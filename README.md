# cpp-tg-bot

C++20 + Boost.Asio 协程实现的异步 Telegram Bot。目标是产出可在任意 Linux 上
直接运行的纯静态二进制。实施计划见 [PLAN.md](PLAN.md)。

## 当前进度

- [x] M1 骨架：协程 HTTPS 客户端 + `getMe` 验证
- [x] M2：Bot API 封装（`apiCall` 统一入口 / 429+5xx 退避重试 / `TelegramApiError`）
- [x] M3：长轮询 + 每 update 协程派发（echobot，`/start` + 回声）
- [x] M4：指数退避重连、429 retry_after 限速、inline 键盘/回调、Ctrl+C 优雅退出
- [x] M5：musl 全静态交付（CI 三平台 release：linux-x86_64-static / macos-arm64 / windows-x86_64，ldd 全静态，strip 后 8.2 MB）
- [x] M6：keep-alive 连接池、按聊天 strand 串行派发（同 chat 严格有序、跨 chat 并发）、offset 持久化（`tg_bot.offset`，原子写，重启不重放丢消息）

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
> cmd //c build-app.bat     # 配置 + 编译，产出 build/Release/tg_bot.exe
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

3. 运行：

```bash
./build/tg_bot <你的token>
# 或
export TG_BOT_TOKEN=<你的token> && ./build/tg_bot
```

看到 `Connected as @<你的bot用户名>` 即 M1 验收通过。

## CI / Release

- 每次 push / PR 跑 [CI](.github/workflows/ci.yml)，三平台矩阵：
  - `linux-static`：Alpine 容器内 musl 工具链，**全静态二进制**
  - `macos-arm64`、`windows-x64`（静态 CRT + vcpkg 静态 triplet）
- 打 tag（如 `v0.1.0`）触发 [Release](.github/workflows/release.yml)，
  自动打包三平台产物并发布到 GitHub Releases

## 技术栈

| 组件 | 用途 |
|------|------|
| Boost.Asio (`co_await`) | 协程 / 事件循环 |
| Boost.Beast | HTTP/1.1 协议解析（header-only） |
| OpenSSL (`ssl::stream`) | TLS，证书链 + 主机名校验 |
| nlohmann-json | Bot API 的 JSON |
| spdlog | 日志 |
| vcpkg | 依赖管理（始终解析仓库内最新版本） |

## 备注

- Linux 静态链接请认准 musl（Alpine）；glibc `-static` 有 NSS 警告，
  不建议用于交付
- Windows 产物目录里的 `cacert.pem` 需要和 exe 放在一起（TLS 校验用）
- macOS 因系统限制无法静态链接系统库，产物为普通动态二进制
