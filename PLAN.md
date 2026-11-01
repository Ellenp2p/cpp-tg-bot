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

**踩坑补充**：
- container job **不会翻译 workflow env 里的路径**：`VCPKG_BINARY_SOURCES`
  写在 env 里时，Alpine 容器内指向主机路径（`/home/runner/work/...`），
  vcpkg 把缓存写进容器里不可见的目录 → 必须在 step 里用
  `$GITHUB_WORKSPACE` 设置（两仓库的 workflow 已统一这么写）
- 首次 push 的 include 目录层级错误（头文件少了 `tgbot/` 子目录）导致
  三平台编译失败；修复后 deps CI 全绿，三平台 bincache 资产齐备
- 实测：命中 bincache 时构建 ~1.5 分钟（原全量 ~6-10 分钟）

## 7. M10 稳定性修复记录（2026-10-31）

> 说明：本文档 §1–§5 是最初的设计稿，目录结构/里程碑编号已与现状不符，
> 现行状态以 README 为准；本节只记录这一轮"代码审查 → 修复"的内容。

**已修（按影响排序）**
1. `tg_bot.offset` 原子写：`std::rename` 在 Windows 上目标存在时直接失败，
   导致 offset 从第二次写开始永远不更新（本机 MSVC 已复现）→ 改用
   `std::filesystem::rename` + `fflush`/`fsync`/`_commit`。
2. CA 证书：编译期绝对路径在发布机上必然不存在，Windows/macOS 会退化成
   空信任库 → 改为 `SSL_CERT_FILE` → exe 同目录 → CWD → 编译期路径，
   找不到时打 error。
3. `getUpdates` 409：残留 webhook 是主因，但日志一直说"另一个进程" →
   自动 `deleteWebhook` 后重试，并区分两种 409。
4. offset 语义：代码一直"启动清空积压"，README 一直写"重启不丢消息" →
   提炼成 `LongPollOptions::dropPendingUpdates`（默认保持旧行为）+ 文档同步。
5. 退出码：启动失败也 return 0 → `run()` 返回 int，失败/永久错误（401/403/404）
   退出码 1；错 token 不再无限重试（只针对 getUpdates，handler 里的 403 仅记
   日志；想一直重试用 `LongPollOptions::permanentErrorPolicy = kRetryForever`）。
6. 限速器：先等后占 → 先占后等（突发时 N 个协程同时发送的问题）。
7. webhook 生命周期：`ssl::context*` / `ChatStrands&` 被 detached 协程裸引用 →
   `shared_ptr` 按值捕获；`path` 改由上层持有。
8. 连接池：进程级静态对象晚于 io_context 析构 → `clearIdlePool()`，
   `BotClient` 析构时清空；池按 host 分键。
9. 写操作重试：`sendMessage` 等非幂等调用在网络错误后重试会造成重复消息 →
   `RetryPolicy::kNoTransportRetry`（只重试 429）。
10. `catch` 类型：`use_awaitable` 抛 `boost::system::system_error`，不是
    `beast::system_error`；原来的连接复用重试是死代码，webhook 还会把正常
    断连刷成 warn。
11. 示例业务：`/silent on` 空快照会导致 `/silent off` 把群权限全置 false
    （永久锁群）→ 空快照直接拒绝 + 先落库快照再禁言；白名单权限补齐
    `can_send_*`；`/silent list` 收归管理员；delay 读取夹取范围；
    callback 没有 `message` 字段时不再抛异常。
12. 工程卫生：`dist2/`、`tg_bot.offset*` 移出版本库并补 `.gitignore`；
    `TGBOT_STATIC` 改为按 triplet 自动推断（带 musl 才开，glibc + 动态
    triplet 加 -static 必链接失败）；CI 静态链接检查改成 `readelf` 硬失败；
    release 改为单 publish job（避免三 job 并发建 release）。

**留待有工具链时再做**
- ~~HTTP 层超时~~、~~Bot API 改 POST~~、~~`builtin-baseline`~~、~~单元测试~~
  → 已在 M11 完成（见下）。

## 8. M11 收尾（2026-11-01）

M10 遗留的四项全部落地，并且这次是**真机编译 + 运行验证**过的
（MSVC 14.44 / vcpkg `d186940b` / Windows 11）。

1. **HTTP 全链路超时**：`Connection` 的流从 `ssl::stream<tcp::socket>` 换成
   `ssl::stream<beast::tcp_stream>`，用它自带的截止时间给
   连接（10s）/ TLS 握手（15s）/ 写（15s）/ 读（按请求给）加超时。
   读超时做成 `HttpRequest::readTimeout`，长轮询传 `timeout + 15s`，
   否则正常的 30 秒静默等待会被自己的超时打断。
   实测：本地起一个"accept 后永不应答"的监听，15.0s 后抛
   `The socket was closed due to a timeout`——正是原先"机器人假死"的根因。
2. **Bot API 改 POST**：`apiCall` 把参数放表单 body，URL 只留
   `/bot<token>/<method>`。4096 字的 `text` 原先百分号编码后能到 ~12KB，
   撞 URL 长度上限就是一发 414。实测假 token 调 `getMe` 仍正确返回 401，
   说明 POST 通路正常。
3. **`vcpkg.json` 固定 `builtin-baseline`**：锁到 deps release 对应的
   `d186940b2f0c4e8e5b4ff804af57d2a26fbfa091`，依赖版本不再随 vcpkg HEAD 漂移。
4. **单元测试**：`tests/tests.cpp` + CTest（不引第三方框架），覆盖
   `Dispatcher` 命令解析 / callback 最长前缀 / `RateLimiter` 时间槽 /
   永久错误分类，20 项断言；CI 三平台加 `ctest` 步骤。
   限速器那条同时是"先占槽后等待"的回归测试（旧实现会测出 0 间隔）。

**仍留在 backlog（非缺陷，属功能增强）**
- 群消息 20 msg/min 的独立限速（框架目前不知道 chat 类型，需调用方配合传入）
- Bot API 的文件上传（`sendPhoto`/`sendDocument` 需要 multipart）
- 自建 Bot API server 支持（host 目前写死 `api.telegram.org`）

