# tgbot — C++20 异步 Telegram Bot 框架

`tgbot` 是一个基于 C++20 协程（Boost.Asio）的 Telegram Bot **框架**：单线程
事件循环、全协程 API、keep-alive 连接池、按聊天串行派发、429 智能退避、
musl 全静态交付。业务机器人作为框架之上的薄壳编写，见 `examples/`。

## 框架结构

```
include/tgbot/     公共头文件（框架 API，#include <tgbot/tg_bot.hpp> 全量引入；
                   SQLite 后端是可选组件，单独 #include <tgbot/sqlite_state_store.hpp>）
src/               框架实现（编译为静态库 tgbot）
examples/
  echo_bot/        演示机器人：回声 / /start / inline 键盘
  group_admin/     群管理机器人：贴纸 60 秒自动过期，/sticker on|off 群管理员控制
  warden/          群务管家：多类型媒体定时清理 + 持久化任务队列 + 拥有者后台
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
- [x] M5：musl 全静态交付（CI 三平台 release，`readelf` 确认无 INTERP 段，strip 后 8.2 MB）
- [x] M6：keep-alive 连接池、按聊天 strand 串行派发（同 chat 严格有序、跨 chat 并发）、offset 持久化（`tg_bot.offset`，原子写）
- [x] M7：框架化（`tgbot` 静态库 + `include/tgbot` 公共头 + examples 结构），新增 `deleteMessage` / `getChatMember` / `co_sleep`，附群管理机器人示例
- [x] M8：webhook 模式（`webhookLoop`，Boost.Beast HTTP/1.1 服务端，TLS 直连或反代两种形态，secret token 校验，与长轮询共用 Dispatcher 和按 chat strand 串行语义）
- [x] M10：稳定性修复（offset 原子写跨平台、CA 证书运行时定位、409 自动清理残留 webhook、永久错误退出、限速器突发超速、webhook 协程生命周期、写操作不重试传输错误）
- [x] M11：Warden 群务管家（`my_chat_member` 群登记、命令前置守卫 `setCommandGuard`、`leaveChat` / `getChatAdministrators`、webhook 显式 `allowed_updates`；调度删除 + 崩溃恢复 + 拥有者私聊后台）

### 长轮询的"停机语义"（重要）

默认行为是**启动时清空停机期间积压的 updates**（"停机即完全停止"，重启不补放
旧消息），`tg_bot.offset` 只是这条路径上的兜底位点。要求"重启后补收未确认消息"
的话，用配置版入口显式打开续传：

```cpp
tgbot::LongPollOptions opts;
opts.dropPendingUpdates = false;          // 从持久化 offset 续传
opts.offsetFile = "/var/lib/mybot/tg_bot.offset";  // 默认 CWD 下的 tg_bot.offset
opts.allowedUpdates = R"(["message","callback_query","edited_message"])";
co_await tgbot::longPollLoop(bot, dispatcher, opts, &gShuttingDown);
```

Telegram 只保留最近 24 小时的 update，所以续传最多补这么久。两种语义都合理，
但必须显式选一个——旧版 README 写的是"重启不丢消息"，代码实际一直在启动时清空。

**永久错误的处理**：`401/403/404`（token 失效 / 被封 / 接口不存在）默认让长轮询
**打一条 error 后退出进程（退出码 1）**，交给 systemd / 容器的重启策略——因为
`getUpdates` 永久失败意味着一条消息都收不到，进程留着只会伪装成"还活着"。
想让它一直退避重试的话：

```cpp
opts.permanentErrorPolicy =
    tgbot::LongPollOptions::PermanentErrorPolicy::kRetryForever;
```

注意作用域：这只管 `getUpdates`。业务 handler 里的 403（被踢出群、没有删消息
权限、回复太老的消息）只会被 `Dispatcher` 记一条日志，**不会**让进程退出。

### webhook 模式

长轮询与 webhook 二选一，应用层代码完全不变（同一个 `Dispatcher`）：

```cpp
tgbot::WebhookConfig cfg;
cfg.publicUrl = "https://bot.example.com/hook";  // 必须 HTTPS（反代形态可用域名）
cfg.secretToken = "<随机串>";   // 强烈建议：校验 X-Telegram-Bot-Api-Secret-Token
cfg.port = 8080;
// cfg.tlsCert / cfg.tlsKey 都设置 → 进程内直接终结 HTTPS（私有化交付）
// 都不设置 → 明文监听 8080，由 Caddy/nginx 终结 TLS 后反代（需透传 secret 头）
co_await tgbot::webhookLoop(bot, dispatcher, cfg, &gShuttingDown);
```

- Telegram 以 HTTP/1.1 POST 投递 update，非 2xx 会自动重试；停机期间回 503 且不再派发，积压由下次启动的 `drop_pending_updates` 清掉（与长轮询"停机即停止"语义一致）
- 监听地址默认 `127.0.0.1`（反代形态不该把明文端口暴露在公网）；要直连或容器内暴露用 `WEBHOOK_HOST=0.0.0.0`
- 本地自测接收端：`registerOnStart = false`（group_admin 用 `TG_WEBHOOK_SKIP_REGISTER=true`），不碰真实 webhook 配置，直接向本地端口伪造 POST 即可
- 与 `getUpdates` 互斥；`maxConnections` 默认 40（Telegram 并发投递上限）
- 从 webhook 模式切回长轮询**不需要手工操作**：长轮询遇到 409 会先 `deleteWebhook` 再重试，日志里会明确写出来

## 踩坑记录（协程必读）

**C++ 协程的引用参数是悬垂引用高发区。** 本项目真实踩过：handler 签名若写
`const json& msg`，调用方传入的临时 `json` 在语句结束时销毁，而协程引用参数
不会拷进协程帧——`co_await` 挂起再恢复后读到的是已释放内存（表现为 key 变空、
值变 null 的随机垃圾，时好时坏）。规则：**跨 `co_await` 存活的对象必须按值传参
或按值捕获**，引用只允许指向协程帧外仍保证存活的对象。

## 快速开始

> Windows + Git Bash 用户注意：Git Bash 不继承 `ProgramFiles(x86)` 等环境变量，
> vcpkg 会因此崩溃（`visualstudio.cpp Value was null`）。仓库里提供了封装好
> VS 环境的脚本，直接用它即可：
>
> ```bash
> cmd //c setup.bat    # 新机器一键初始化（见下）
> cmd //c build-app.bat # 日常开发：增量编译，产出 build/Release/{echo_bot,group_admin,warden}.exe
> ```

1. 在 Telegram 找 [@BotFather](https://t.me/BotFather) 创建 Bot，拿到 token
2. 本地构建。**新机器推荐 `setup.bat`**：自动 clone vcpkg、从
   [tgbot deps release](https://github.com/Ellenp2p/tgbot/releases/tag/deps)
   下载预编译依赖（并检出对应 vcpkg commit 保证缓存命中），一分钟内就绪；
   拉取失败会自动回退本地全量编译。日常开发直接 `build-app.bat`（增量编译）。

手动构建（所有平台）：

```bash
# 可选：先拉预编译依赖（和 CI 同款，省去编译 Boost/OpenSSL）
base=https://github.com/Ellenp2p/tgbot/releases/download/deps
curl -fsSL -o bincache.tar.gz "$base/vcpkg-bincache-<triplet>.tar.gz"
curl -fsSL -o vcpkg-commit.txt "$base/vcpkg-commit.txt"
tar xzf bincache.tar.gz   # 解出 vcpkg-bincache/
export VCPKG_BINARY_SOURCES="files,$PWD/vcpkg-bincache,readwrite"

# clone vcpkg；若拉了预编译包，须检出 vcpkg-commit.txt 里同一 commit
git clone --depth 1 https://github.com/microsoft/vcpkg.git .vcpkg
if [ -f vcpkg-commit.txt ]; then
  sha=$(cat vcpkg-commit.txt)
  git -C .vcpkg fetch --depth 1 origin "$sha" && git -C .vcpkg checkout --detach "$sha"
fi

# Linux / macOS
./.vcpkg/bootstrap-vcpkg.sh -disableMetrics
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake"
cmake --build build

# Linux 想要全静态（musl）：在 Alpine 里加 overlay triplet 并显式打开 TGBOT_STATIC
# cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
#       -DCMAKE_TOOLCHAIN_FILE="$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake" \
#       -DVCPKG_OVERLAY_TRIPLETS="$PWD/triplets" \
#       -DVCPKG_TARGET_TRIPLET=x64-linux-musl -DTGBOT_STATIC=ON
# TGBOT_STATIC 默认按 triplet 自动推断：带 musl 的 triplet 自动打开，否则关闭
# （glibc + 动态 triplet 加 -static 会报 attempted static link of dynamic object）

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

### Warden（群务管家）

`examples/warden/` 是比 `group_admin` 更完整的群管机器人：贴纸/图片/视频/语音
等媒体按类型、按群设置时限自动删除；任务落 SQLite，停机/崩溃后重启会把逾期
任务补删；拥有者私聊 `/panel` 查看机器人所在的所有群、权限清单、启停功能、
取消待删、退群。群内命令与回复走 ephemeral（仅操作者可见）；**群里的命令仅
群管理员可用，非管理员发来的命令会被静默忽略**（`Dispatcher::setCommandGuard`
实时校验 `getChatMember`，不回复、不产生副作用）；命令菜单也按
`chat_administrators` 作用域发布，普通成员在群里连 `/` 菜单都看不到这些命令。
删除连续失败可同时告警到 owner 私聊、飞书自定义机器人、Discord webhook。

```bash
TG_BOT_TOKEN=<token> WARDEN_OWNERS=<你的用户ID> ./build/Release/warden
```

完整命令、环境变量、架构与已知限制见
[examples/warden/README.md](examples/warden/README.md)；零停机热更新设计见
[examples/warden/HOT_UPDATE.md](examples/warden/HOT_UPDATE.md)。

## 富文本与流式输出

**富文本**走 Bot API 的 `parse_mode`，用 `SendOptions` 指定：

```cpp
tgbot::SendOptions so;
so.parseMode = tgbot::ParseMode::kHtml;   // 或 kMarkdownV2
so.disableWebPagePreview = true;
co_await bot.sendMessage(chatId, "<b>粗体</b> 和 <code>代码</code>", so);
```

程序拼接内容**优先用 HTML**：只需转义 `& < >`（`tgbot::escapeHtml`）；
MarkdownV2 有 18 个元字符要转义（`tgbot::escapeMarkdownV2`），漏一个整条消息就 400。

单条消息上限 4096 字符，`tgbot::splitMessage` 负责切块：优先在换行/空格处断开，
不切断 UTF-8 多字节字符，HTML 模式下还会自动补闭合标签、下一段重新打开。

**流式输出**（Telegram 没有流，只能发一条再反复编辑）：

```cpp
tgbot::StreamMessage stream(bot);
co_await stream.begin(chatId);                 // 先发占位（带光标 ▍）
while (auto delta = provider.next()) {
    co_await stream.append(delta);             // 想多快就多快，内部自动节流
}
co_await stream.finish();                      // 去掉光标、做最终渲染
```

内部处理了四个坑：编辑节流（默认 1.2s，Telegram 编辑有频率限制）、
内容未变化时不发（否则 400 `message is not modified`）、消息被删/不可编辑时
自动停止且不打断生成、超过 4096 字符时冻结当前消息并在收尾时分条续发。

`examples/echo_bot` 里有现成演示：`/rich` 展示各种 HTML 标签，
`/stream` 模拟逐段生成。

## 消息操作台（Panel 抽象）

一条消息可以当"操作台"用：点按钮 → 改状态 → **原地编辑同一条消息**。
框架把它抽成三层，业务只写状态机：

| 层 | 职责 |
|---|---|
| `Panel`（抽象基类） | `render()` 出文本+键盘；`onClick()` 改状态、往 `PanelEffects` 填副作用。**不做 I/O、不是协程**，所以能单测 |
| `PanelEffects` | 提示气泡 / 只有某人可见的私密消息 / 额外公开消息，由 manager 统一执行 |
| `PanelManager` | 发消息、按 `p:<action>` 路由回调、先应答再编辑、跑倒计时 ticker |

```cpp
tgbot::PanelManager panels(bot);
net::co_spawn(executor, panels.runTicker(), net::detached);  // 倒计时/自动开奖

dispatcher.onCallbackQuery("p:", [&panels](tgbot::BotClient&, const json cq)
        -> net::awaitable<void> { co_await panels.handleCallback(cq); });

// 发一个抽奖面板
tgbot::LotteryPanel::Config cfg;
cfg.creatorId = msg["from"].value("id", std::int64_t{0});  // 只有发起者能手动开奖
cfg.winnerCount = 1;
cfg.duration = std::chrono::seconds{120};
co_await panels.open(chatId, std::make_shared<tgbot::LotteryPanel>(cfg));
```

内置的 `LotteryPanel` 覆盖了：参与去重、发起者限权开奖、到点自动开奖。
中奖名单**直接在群里公布**（`rewardPrivate=true` 时才会额外私聊发专属码）。

### 群红包（`RedPacketPanel`）

消息本身就是红包，抢到多少、谁抢的**全部在群里可见**，不需要去私聊找机器人：

```text
🧧 恭喜发财，大吉大利

总金额：¥100.00
剩余：¥62.45（3/5 已领取）

@alice  ¥12.34
@bob    ¥25.21
@carol  ¥25.10
```

- 点「🧧 抢红包」当场分钱，消息原地刷新名单；抢完按钮自动消失
- 金额用**整数分**计算（无浮点误差），分配走**二倍均值法**（和微信一致，避免有人独吞）
- 发起人不能抢自己的；同一个人重复点不会重复领（还会告诉你上次抢了多少）
- 默认 1 小时有效期，过期显示"已过期 + 未领取金额"，按钮同时消失

```cpp
tgbot::RedPacketPanel::Config cfg;
cfg.totalCents = 10000;   // ¥100.00
cfg.totalCount = 5;
cfg.creatorId = msg["from"].value("id", std::int64_t{0});
co_await panels.open(chatId, std::make_shared<tgbot::RedPacketPanel>(cfg));
```

**响应速度**：点击后先 `answerCallbackQuery`（客户端立刻停掉转圈），再编辑消息；
同一个面板的点击由该聊天的 strand 串行处理，不会并发改同一条消息。
`editMessageText` 与发送共用同一套限速器，所以高频点击会被合并、不会打爆 Telegram。

**局限（下一步）**：面板状态在内存里，进程重启后丢失；持久化可以挂 `StateStore`
（`Panel::type()` 已预留）。`sendEphemeralMessage` 目前不带 `parse_mode`，
所以私密消息用纯文本。

### 没领完的红包怎么退（持久化队列 + 批量退款）

"钱"不能只活在内存里，否则进程一重启就丢。框架给了三块：

| 组件 | 作用 |
|---|---|
| `RedPacketLedger` | 账本：总额/剩余/每笔领取。领取是带条件的原子 UPDATE，不可能超发 |
| `JobQueue` | 持久化任务队列：延迟执行、失败指数退避重试、超限进死信 |
| `RefundService` | 把"当前所有已到期"的红包合并成一批，一次 handler 调用退完 |

流程：

1. `createRedPacket()` 落账 + 排一条 `red_packet_refund` 任务（`run_at = 到期时刻`）
2. 到期后 worker 领任务 → `RefundService::flush()` 抓取**所有**可退红包
3. 逐个 `beginRefund()` 抢退款权（`open/expired → refunding`，**只会成功一次**）
4. 只调**一次** `BatchRefundHandler`，链上就是一次批量转账
5. 成功的 `finishRefund()` 落 `refunded` + tx hash；失败的退回 `expired` 等下一轮

```cpp
auto ledger = std::make_shared<tgbot::RedPacketLedger>("red_packets.db");
tgbot::JobQueue jobs("jobs.db");
tgbot::RefundService refunds(*ledger, tgbot::RefundService::Options{});  // decimals=18

const tgbot::BatchRefundHandler refund =
    [](const tgbot::RefundBatch& batch) -> net::awaitable<tgbot::RefundBatchResult> {
    co_await chain.payout(batch.items);   // 一次交易打给多个人（multisend）
    tgbot::RefundBatchResult result;
    result.ref = txHash;                  // 写回每个红包的 tx_ref
    co_return result;                     // 抛异常 = 整批重试
};
net::co_spawn(ex, jobs.runWorker(
    [&](const tgbot::JobRecord& job) -> net::awaitable<void> {
        co_await refunds.handleJob(job, refund);
    }), net::detached);
```

**批量的意义**：每个红包都排了任务，但实际只会发生一次转账 —— 第一个任务把同批
全处理掉，后面的任务发现状态已变直接空跑。`maxBatch` 控制单批上限；
`failedPacketIds` 可以让个别失败的红包单独重试。

### 金额为什么用 `Amount`（128 位）

`int64` 存 wei 会溢出：

```
1 ETH     = 1 000 000 000 000 000 000 wei
int64 上限 = 9 223 372 036 854 775 807   ≈ 9.22 ETH
```

`Amount` 是 128 位无符号（MSVC 没有 `__int128`，所以用两个 64 位 limb 手写），
提供 `parse / toString / 加减 / mulSmall / divmod`，**溢出抛异常而不是静默回绕**：

```cpp
auto wei = tgbot::Amount::fromCents(100, 18);  // ¥1.00 → 1e18 wei
wei->toString();                                // "1000000000000000000"
```

为什么这套接链上是安全的：

- **任务落库**：重启后照样会退款，不会出现"钱卡在机器人里"
- **幂等**：`key` 唯一 + 账本状态机，重试不会重复打款（双花）
- **可对账**：`tx_ref` 记下 tx hash，崩溃后能追
- **不静默丢**：退不动的任务进 `dead` 死信，有 `countDead()` 可以告警

### 对账与熔断（接链必备）

**对账**补的是"广播成功但进程崩了"的窗口：

- `TxRefRecorder`：拿到 tx hash 的**第一时间**就写进 `tx_ref`，别等确认
- `Reconciler` 每 60 秒扫一遍卡在 `refunding` 的红包，按 hash 回查链：

| 链上结果 | 动作 |
|---|---|
| `confirmed` | 标记 `refunded`（钱其实已经退了，绝不重发）|
| `failed` | 退回 `expired`，重新排队 |
| `not_found` | 短期 → 重排；超过 `stuckAfterSeconds` → 标 `stuck` + 告警 |
| `pending` | 下一轮再看 |
| 探测抛异常 | 熔断计数 +1、告警、本轮中止 |

**熔断**：`ChainMonitor` 连续 5 次失败 → `paused`，此时 `flush()` 直接拒绝执行，
避免 RPC 挂掉时疯狂重试把队列打爆；对账探测成功一次就自动恢复。

**重试天花板**：单笔退款超过 `maxAttempts`（默认 5）→ 标 `stuck` + 告警，
不再自动重试，等人确认链上到底发生了什么。

```cpp
tgbot::ChainMonitor chain;
tgbot::RefundService refunds(*ledger, chain, opts, alert);
tgbot::Reconciler reconciler(*ledger, chain, tgbot::Reconciler::Options{});

// 每 60 秒对一次账
co_await reconciler.run(
    [](std::string_view txHash) -> net::awaitable<tgbot::ChainTxStatus> {
        co_return co_await chain.rpc.txStatus(txHash);  // confirmed/failed/pending
    },
    alert);
```

## 测试

自带一组不依赖网络的单元测试（CTest，不需要第三方测试框架）：

```bash
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure   # 或直接跑 build/Release/tgbot_tests
```

覆盖：`Dispatcher` 的命令解析（`/cmd@Bot` 归一化、过滤发给其他 bot 的命令）、
callback 最长前缀匹配、`RateLimiter` 的时间槽间隔（同聊天 ≥1s、全局 ≥~34ms，
同时是"先占槽后等待"的回归测试）、永久错误分类。

## CI / Release

- 每次 push / PR 跑 [CI](.github/workflows/ci.yml)，三平台矩阵：
  - `linux-static`：Alpine 容器内 musl 工具链，**全静态二进制**
  - `macos-arm64`、`windows-x64`（静态 CRT + vcpkg 静态 triplet）
- 发布触发 [Release](.github/workflows/release.yml)，两种方式：
  - **手动（推荐）**：Actions → Release → Run workflow，选择机器人
    （group_admin / echo_bot / warden / both）+ 版本号即可，不用手打 tag
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
| vcpkg | 依赖管理（`vcpkg.json` 固定 `builtin-baseline`，版本可复现） |

## 备注

- **HTTP 细节**：所有 Bot API 调用走 `POST application/x-www-form-urlencoded`
  （不再把参数拼进 URL，长文本/大键盘不会撞 URL 长度上限）。全链路都有超时：
  连接 10s、TLS 握手 15s、写 15s、读由请求指定（长轮询取
  `timeout + 15s`），对端黑洞不会再让机器人静默假死
- **退出码**：启动失败（错 token、端口被占、webhook 注册失败）退出码是 1；
  运行中遇到 401/403/404 这类永久错误也会直接退出而不是无限重试，
  方便 systemd `Restart=on-failure` / 容器重启策略生效
- **CA 证书查找顺序**：`SSL_CERT_FILE` → 可执行文件同目录 `cacert.pem` →
  当前工作目录 `cacert.pem` → 构建期路径；全都找不到时启动日志会打 error
  （此时 TLS 一定失败）。发布包已经把 `cacert.pem` 放在 exe 旁边
- Linux 静态链接请认准 musl（Alpine）；glibc `-static` 有 NSS 警告，
  不建议用于交付。`TGBOT_STATIC` 默认按 triplet 自动推断（名里带 `musl` 才打开），
  仍可用 `-DTGBOT_STATIC=ON/OFF` 强制覆盖
- macOS 因系统限制无法静态链接系统库，产物为普通动态二进制
- 运行时状态文件（`tg_bot.offset` / `group_admin_state.db*`）属于部署数据，
  已加入 `.gitignore`，别再提交进仓库

