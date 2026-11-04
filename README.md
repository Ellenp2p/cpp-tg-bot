# tgbot — C++20 异步 Telegram Bot 框架

基于 C++20 协程和 Boost.Asio 的 Telegram Bot 框架：单线程事件循环、协程 API、
长轮询 / webhook 双模式、按聊天串行派发、限速与重试、SQLite 持久化、静态发布。

仓库内包含可运行的示例机器人：

| 示例 | 说明 |
|---|---|
| `examples/echo_bot` | 回声、`/start`、inline 键盘 |
| `examples/group_admin` | 群贴纸定时清理 |
| `examples/warden` | 群务管家：多类型媒体清理、豁免、后台、监控 |

Warden 的完整说明见 [examples/warden/README.md](examples/warden/README.md)。

## 目录

```text
include/tgbot/       框架公共头文件
src/                 框架实现（静态库 tgbot）
examples/            echo_bot / group_admin / warden
deploy/              Warden systemd 与 Prometheus/Grafana 配置
tests/               框架与 Warden 回归测试
```

## 最小示例

```cpp
#include <tgbot/tg_bot.hpp>

boost::asio::awaitable<void> run(const std::string& token) {
    tgbot::BotClient bot(token);
    tgbot::Dispatcher dispatcher;

    dispatcher.onAnyMessage([](tgbot::BotClient& bot, nlohmann::json msg)
            -> boost::asio::awaitable<void> {
        const auto chatId = msg["chat"].value("id", std::int64_t{0});
        co_await bot.sendMessage(chatId, "You said: " +
                                         msg.value("text", std::string{}));
    });

    co_await tgbot::longPollLoop(bot, dispatcher);
}
```

约定：handler 是协程，**跨 `co_await` 存活的数据必须按值持有**，不要保存
临时 `json` 的引用。

## 构建

依赖通过 vcpkg 管理，版本固定在 `vcpkg.json`。

### Windows

```powershell
cmd /c setup.bat       # 首次：准备 vcpkg 和预编译依赖
cmd /c build-app.bat   # 增量编译
```

产物：

```text
build\Release\echo_bot.exe
build\Release\group_admin.exe
build\Release\warden.exe
```

### Linux / macOS

```bash
git clone https://github.com/microsoft/vcpkg.git .vcpkg
./.vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build
```

Linux 全静态构建使用 Alpine + musl triplet；`TGBOT_STATIC` 会按 triplet
自动推断，也可用 `-DTGBOT_STATIC=ON` 强制。

### 测试

```bash
cmake --build build --config Release --target warden_tests

# Linux / macOS
./build/warden_tests

# Windows
.\build\Release\warden_tests.exe
```

## 运行

```bash
./build/warden <bot-token>
# 或
export TG_BOT_TOKEN=<token>
export WARDEN_OWNERS=<你的用户ID>
./build/warden
```

环境变量、Warden 命令、数据库和部署说明见
[examples/warden/README.md](examples/warden/README.md)。

## 框架能力

- 协程化 Bot API 调用、HTTP keep-alive、429 / 5xx 退避重试；
- 长轮询与 webhook 共用 Dispatcher，支持 `allowed_updates`、offset 持久化；
- 按聊天串行、跨聊天并发的 worker；
- inline 键盘、callback、富文本、流式编辑；
- Panel / Lottery / RedPacket 等可复用消息面板；
- SQLite `StateStore`、JobQueue、对账与熔断组件；
- Prometheus `/metrics`、systemd 部署示例。

具体 API 以 `include/tgbot/` 头文件和示例代码为准。

## CI / Release

- `.github/workflows/ci.yml`：Linux static、macOS arm64、Windows x64；
- `.github/workflows/release.yml`：tag 或手动触发，发布对应机器人；
- Warden 当前 release：`warden-v0.2.2`。

## 安全与运行时数据

- 不提交 token、webhook、密码、私钥或生产数据库；
- `.gitignore` 已排除 `*.db`、`*.offset`、`logs/`、构建产物和 vcpkg 缓存；
- 生产环境使用环境变量或 systemd `EnvironmentFile` 注入密钥；
- 公开仓库中的示例 webhook / ID 都是占位符，不能直接用于生产。

## 文档

- [Warden 使用说明](examples/warden/README.md)
- [Warden 开发规则](agent.md)
- [监控配置](deploy/monitoring/)
- [systemd 配置](deploy/warden/)
