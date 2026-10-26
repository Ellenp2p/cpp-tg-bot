# tgbot — C++20 异步 Telegram Bot 框架

`tgbot` 是一个基于 C++20 协程（Boost.Asio）的 Telegram Bot **框架**：单线程
事件循环、全协程 API、keep-alive 连接池、按聊天串行派发、429 智能退避、
musl 全静态交付。业务机器人作为框架之上的薄壳编写，见 `examples/`。

## 框架结构

```
include/tgbot/     公共头文件（框架 API，#include <tgbot/tg_bot.hpp> 全量引入）
src/               框架实现（编译为静态库 tgbot）
examples/
  echo/            演示机器人：回声 / /start / inline 键盘
  group_admin/     群管理机器人：贴纸 60 秒自动过期，/sticker on|off 群管理员控制
```

最小机器人长这样：

```cpp
#include <tgbot/tg_bot.hpp>

boost::asio::awaitable<void> run(const std::string& token) {
    tgbot::BotClient bot(token);
    tgbot::Dispatcher dispatcher;
    dispatcher.onAnyMessage([](tgbot::BotClient& bot, const nlohmann::json msg)
            -> boost::asio::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        co_await bot.sendMessage(chatId, "You said: " +
                                         msg.value("text", std::string{}));
    });
    co_await tgbot::longPollLoop(bot, dispatcher);
}
```

框架约定：**handler 是协程，json 按值传参**（跨 `co_await` 存活的对象必须
按值持有，这是血泪教训，见下方踩坑记录）。

## 当前进度

- [x] M1 骨架：协程 HTTPS 客户端 + `getMe` 验证
- [x] M2：Bot API 封装（`apiCall` 统一入口 / 429+5xx 退避重试 / `TelegramApiError`）
- [x] M3：长轮询 + 每 update 协程派发（echobot，`/start` + 回声）
- [x] M4：指数退避重连、429 retry_after 限速、inline 键盘/回调、Ctrl+C 优雅退出
- [x] M5：musl 全静态交付（CI 三平台 release，ldd 全静态，strip 后 8.2 MB）
- [x] M6：keep-alive 连接池、按聊天 strand 串行派发（同 chat 严格有序、跨 chat 并发）、offset 持久化（`tg_bot.offset`，原子写，重启不丢消息）
- [x] M7：框架化（`tgbot` 静态库 + `include/tgbot` 公共头 + examples 结构），新增 `deleteMessage` / `getChatMember` / `co_sleep`，附群管理机器人示例

运行时会生成 `tg_bot.offset`（长轮询确认位点，重启续传用），别删。

## 踩坑记录（协程必读）

**C++ 协程的引用参数是悬垂引用高发区。** 本项目真实踩过：handler 签名若写
`const json& msg`，调用方传入的临时 `json` 在语句结束时销毁，而协程引用参数
不会拷进协程帧——`co_await` 挂起再恢复后读到的是已释放内存（表现为 key 变空、
值变 null 的随机垃圾，时好时坏）。规则：**跨 `co_await` 存活的对象必须按值传参
或按值捕获**，引用只允许指向协程帧外仍保证存活的对象。

## 快速开始

> Windows + Git Bash 用户注意：Git Bash 不继承 `ProgramFiles(x86)` 等环境变量，
> vcpkg 会因此崩溃（`visualstudio.cpp Value was null`）。仓库里提供了两个
> 封装好 VS 环境的脚本，直接用它俩即可：
>
> ```bash
> cmd //c build-deps.bat    # 装依赖（首次约 15 分钟，有缓存后秒级）
> cmd //c build-app.bat     # 配置 + 编译，产出 build/Release/{echo_bot,group_admin}.exe
> ```

1. 在 Telegram 找 [@BotFather](https://t.me/BotFather) 创建 Bot，拿到 token
2. 本地构建（首次会编译 Boost/OpenSSL，约 20–60 分钟，之后有缓存很快）：

```bash
# 所有平台通用：clone vcpkg（提供最新版依赖）
git clone --depth 1 https://github.com/microsoft/vcpkg.git .vcpkg

# Linux / macOS
./.vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build

# Windows（Developer PowerShell / Git Bash 均可）
.\.vcpkg\bootstrap-vcpkg.bat -disableMetrics
cmake -B build -DCMAKE_TOOLCHAIN_FILE="$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake" `
      -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
```

3. 运行（以群管理机器人为例）：

```bash
./build/group_admin <你的token>
# 或
export TG_BOT_TOKEN=<你的token> && ./build/group_admin
```

看到 `Connected as @<你的bot用户名>` 即启动成功。把它拉进群、设为管理员后，
群里发 `/sticker on` 开启贴纸 60 秒自动过期，`/sticker off` 关闭
（仅群管理员可开关，设置存 `group_admin_state.db`，SQLite 持久化，重启不丢）。

## CI / Release

- 每次 push / PR 跑 [CI](.github/workflows/ci.yml)，三平台矩阵：
  - `linux-static`：Alpine 容器内 musl 工具链，**全静态二进制**
  - `macos-arm64`、`windows-x64`（静态 CRT + vcpkg 静态 triplet）
- 发布触发 [Release](.github/workflows/release.yml)，两种方式：
  - **手动（推荐）**：Actions → Release → Run workflow，选择机器人
    （group_admin / echo_bot / both）+ 版本号即可，不用手打 tag
  - **打 tag**：`group_admin-v0.0.1` / `echo_bot-v0.0.1` 只发对应机器人，
    `v0.2.0` 发全部
- 只编译目标机器人（tgbot 库作为依赖自动编，另一个示例不编）；
  产物命名：`<bot>-<platform>.tar.gz`（Linux/macOS，保留可执行权限 + cacert.pem）
  或 `<bot>-<platform>.exe`（Windows，下载即跑）
- **依赖预编译**：框架源码独立维护在 [Ellenp2p/tgbot](https://github.com/Ellenp2p/tgbot)，
  其 deps CI 每日检查上游、且**只在我们依赖的 port（openssl / sqlite3 /
  spdlog / fmt / nlohmann-json / boost-*）有更新时才重编**，
  预编译 binary cache 发布到 [`deps` release](https://github.com/Ellenp2p/tgbot/releases/tag/deps)。
  本仓库 CI 先拉取 bincache 并检出同一 vcpkg commit（ABI 哈希一致），
  `vcpkg install` 变为秒级恢复；拉取失败自动回退全量编译

## 技术栈

| 组件 | 用途 |
|------|------|
| Boost.Asio (`co_await`) | 协程 / 事件循环 |
| Boost.Beast | HTTP/1.1 协议解析（header-only） |
| OpenSSL (`ssl::stream`) | TLS，证书链 + 主机名校验 |
| nlohmann-json | Bot API 的 JSON |
| spdlog | 日志 |
| SQLite3 | 状态持久化（`StateStore` 接口，见 `tgbot/state_store.hpp`） |
| vcpkg | 依赖管理（始终解析仓库内最新版本） |

## 备注

- Linux 静态链接请认准 musl（Alpine）；glibc `-static` 有 NSS 警告，
  不建议用于交付
- Windows 产物目录里的 `cacert.pem` 需要和 exe 放在一起（TLS 校验用）
- macOS 因系统限制无法静态链接系统库，产物为普通动态二进制
