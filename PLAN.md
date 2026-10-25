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

### M5：静态化交付（1 天）

路线（二选一，先试 A）：

- **A. 本机 vcpkg musl triplet**：
  `vcpkg install --triplet=x64-linux-musl`，CMake 配
  `VCPKG_TARGET_TRIPLET=x64-linux-musl` + `-static -static-libgcc -static-libstdc++`
  产出 `ldd` 显示 "not a dynamic executable"
- **B. Docker（推荐作为 CI/最终交付）**：Alpine 容器内 `apk add` 依赖后编译，
  产物拷出；本机无需装 musl 工具链

- 验收：单文件二进制 ≤ 5 MB，在干净 Linux（如 `ubuntu:latest` 容器）里运行成功

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
