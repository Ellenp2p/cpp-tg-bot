# cpp-tg-bot 实施计划

> 目标：用 C++20 + Boost.Asio 协程，从零实现一个异步 Telegram Bot，
> 最终产出可在任意 Linux 上运行的纯静态二进制。

## 0. 核心决策（已定）

| 决策点 | 结论 | 理由 |
|--------|------|------|
| 语言标准 | C++20（`co_await` / `co_spawn`） | 协程原生支持 |
| 协程/事件循环 | Boost.Asio（`awaitable<T>`） | 最成熟的协程落地，header-only 居多 |
| Bot API | **自己封装**，不用 tgbot-cpp | 代码风格现代、依赖少、可静态化 |
| HTTP/TLS | 自己基于 Asio 写 HTTPS 客户端 | 全协程、无 libcurl；TLS 用 `ssl::stream`（OpenSSL 后端） |
| JSON | nlohmann/json | 单头文件 |
| 日志 | spdlog | 单头文件 |
| 包管理 | vcpkg（manifest 模式） | 个人项目 + CMake + macOS 开发 / Linux 部署的最优解 |
| 部署形态 | musl 静态 triplet 编译，产出单文件二进制 | 拷到任何 Linux 即可运行 |

依赖清单（`vcpkg.json`）：`boost-asio`、`openssl`、`nlohmann-json`、`spdlog`。
要求 Boost ≥ 1.75（协程完整支持），直接锁定 1.85+。

## 1. 架构

```
main
 └─ io_context（先单线程，M4 再评估多线程）
     ├─ BotClient     ── HTTPS + Bot API 方法封装（co_await）
     ├─ LongPollLoop  ── 长轮询协程（timeout=30s，offset 管理）
     └─ Dispatcher    ── 每 update co_spawn 一个处理协程
         └─ Handler 注册表：onCommand / onAnyMessage / onCallbackQuery
```

关键设计：

1. 每个 update 一个协程（`co_spawn`），单个用户阻塞不影响其他人
2. 一个长轮询协程 + N 个处理协程，无线程池
3. 起步单 HTTPS 连接串行复用（keep-alive），M4 再考虑连接池

## 2. 目录结构

```
tg-bot/
├── CMakeLists.txt
├── vcpkg.json                  # 依赖清单（manifest 模式）
├── Dockerfile                  # Linux 静态构建（备选/交付用）
├── src/
│   ├── main.cpp                # 入口、io_context、信号处理
│   ├── http_client.{h,cpp}     # 底层协程 HTTPS 客户端
│   ├── bot_client.{h,cpp}      # Bot API 封装（apiCall/sendMessage/getUpdates/...）
│   ├── long_poll.{h,cpp}       # 长轮询循环 + 断线重连
│   ├── dispatcher.{h,cpp}      # 事件注册 + 协程派发
│   └── handlers.cpp（可选）    # 具体业务 handler 示例
└── PLAN.md                     # 本文件
```

## 3. 里程碑

### M1：协程 HTTPS 客户端骨架（1–2 天）

- 实现 `co_await httpsRequest(host, path)`：
  - `ssl::stream` 建立连接（`async_connect` + 异步 `handshake`）
  - **证书校验用默认 verify，不关闭**
  - HTTP/1.1 请求拼装、响应解析（状态行 + headers + body）
  - keep-alive 连接复用
- 验收：`co_await` 调 `getMe`，打印 bot 用户名

### M2：Bot API 最小封装（1 天）

- `BotClient` 类：`apiCall(method, params) -> json` 统一入口
- 实现 `getUpdates(offset, timeout)`、`sendMessage(chatId, text)`
- Telegram 错误响应处理（`ok=false`、`error_code`、429/5xx 重试）
- 验收：命令行手动收发消息成功

### M3：长轮询 + 协程派发（1 天）

- 长轮询循环协程：`offset` 持久化、30s timeout
- `Dispatcher`：`onCommand("start", fn)` / `onAnyMessage(fn)` 注册
- 每 update `co_spawn` 派发
- **所有处理协程包 try/catch**，异常记日志不 terminate
- 验收：echobot（/start 回复 + 回声）跑通

### M4：打磨（2–3 天）

- 断线重连：指数退避 1s→2s→4s…封顶 30s；HTTP 409（多实例同 token）单独处理
- 发送限速：令牌桶 ~30 msg/s 全局、1 msg/s 单聊天
- 功能补全：inline/reply 键盘、callback query、文件收发（先下载后上传）
- 日志：spdlog，分级 + 写文件
- 优雅退出：SIGINT/SIGTERM 停 io_context
- 验收：连续运行 24h 无内存泄漏（可用 `valgrind --leak-check=summary` 抽查）

### M5：静态化交付（1 天）—— ✅ 已完成

实际路线：**GitHub Actions（release.yml）三平台矩阵**，推 `v*` tag 自动出产物：

- linux-static：`alpine:3.22` 容器（job container 直接构建，无 DinD）+
  overlay triplet `triplets/x64-linux-musl.cmake`（vcpkg 已移除内置 musl triplet）+
  `VCPKG_FORCE_SYSTEM_BINARIES=1`（vcpkg 官方 cmake 4.4.3 是 glibc 版，musl 跑不了）
  + `pkgconf`（`vcpkg_fixup_pkgconfig` 需要）+ `-static` + strip
- macos-arm64 / windows-x64：官方 runner + vcpkg 静态 triplet（Windows 额外 /MT）

验收结果（v0.1.1）：`ldd` → "not a dynamic executable"；musl 静态二进制
在 glibc Ubuntu 上直接运行成功；strip 后二进制 8.2 MB（压缩包 3.3 MB）。
说明：5 MB 预算低估了静态 OpenSSL（libcrypto 全算法集本身就 ~5 MB），
8 MB 级是纯静态 + TLS 的正常体积（可参考静态编译的 Go 程序 ~7–9 MB）。

**CI 踩坑记录**（全部已修，见 git 历史）：
1. Alpine 无 bash → 安装步骤显式 `shell: sh`
2. 容器内不能再 `docker run`（无 DinD）→ 直接在 job container 里构建
3. `Invalid triplet: x64-linux-musl` → 仓库自带 overlay triplet
4. vcpkg 下载 glibc 版 cmake 在 musl 上无法运行 → `VCPKG_FORCE_SYSTEM_BINARIES`
5. nlohmann-json 构建失败 → 补装 `pkgconf`
6. 二进制 12 MB（含 debug info）→ 打包前 `strip`

## 4. 已知坑（按踩坑概率排序）

1. **协程帧生命周期**：`awaitable` 捕获的引用在挂起期间必须存活；`ssl::stream`
   归属对象要明确，别悬空
2. **`co_spawn` 异常 = terminate**：处理协程入口统一 try/catch
3. **TLS 握手必须异步**：用 `co_await` 版本，别混同步调用
4. **单 `ssl::stream` 不可并发写**：多协程发消息要走 strand 或写队列串行化
5. **先动态链接跑通，最后再静态化**：musl 交叉编译放到最后一步
6. **macOS 无法静态链接系统库**：静态化只能在 Linux 侧做
7. **glibc `-static` 有 NSS 警告**：完全静态认准 musl，别用 glibc 凑合

## 5. 执行顺序

```
M1 → M2 → M3（此时已是一个可跑的 Bot）
  → M4（生产可用）
  → M5（交付形态）
```

每个里程碑结束时编译 + 用真实 token 验证，再进下一个。


## 6. M8 / M9 追加记录（2026-10-26）

**M8：SQLite 状态存储**
- 新增 `tgbot::StateStore` 抽象（`get` / `set` / `erase`，见
  `include/tgbot/state_store.hpp`）+ SQLite 实现 `SqliteStateStore`（WAL，
  prepared statements）
- group_admin 的群开关改存 `group_admin_state.db`；vcpkg 新增 `sqlite3` 依赖
- 注意：vcpkg 的 sqlite3 CMake target 名是 `unofficial::sqlite3::sqlite3`
  （双冒号命名空间），不是 `unofficial-sqlite3::sqlite3`

**M9：框架独立仓库 + 预编译依赖**
- 框架源码 → [Ellenp2p/tgbot](https://github.com/Ellenp2p/tgbot)（public，
  只含 include/ src/ CMakeLists vcpkg.json triplets/）
- `deps.yml`：push / 每日 03:23 UTC / 手动触发，编译 vcpkg 依赖并把
  binary cache 发到 tag `deps` 的 rolling release
  （`vcpkg-bincache-<triplet>.tar.gz` + `vcpkg-commit.txt`）
- 本仓库 CI/release 构建前先 curl 拉 bincache 并按 `vcpkg-commit.txt`
  浅克隆检出同一 commit（ABI 哈希一致才命中），失败自动回退全量编译
- release 产物：单个 tarball 内含 `echo_bot/` 与 `group_admin/` 两个目录
- 本地 `framework/` 目录是 tgbot 仓库的工作副本（已 gitignore，
  不进入本仓库）
