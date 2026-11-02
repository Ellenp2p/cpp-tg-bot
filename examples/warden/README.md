# Warden · 群务管家

> 名字：**Warden**（看守者）。一个盯着群、按点清理、还能让拥有者看清"我在哪些群、
> 权限够不够"的 C++ 群管机器人。可执行文件就叫 `warden`，目录 `examples/warden/`。
>
> 备选名（都没采用，但改起来只是重命名目录/目标）：Vigil（守夜）、Custos（拉丁语
> "守卫"）、Steward（管家）。

## 1. 现在能做什么（v0.1）

| 能力 | 状态 | 说明 |
|---|---|---|
| 贴纸 / 图片 / 视频 / 语音定时删除 | ✅ | 另支持音频、动图、圆视频 |
| 删除范围 | ✅ | 所有媒体消息，**不区分发送者（管理员也删）**；文本/链接/文件不在范围 |
| 置顶消息保护 | ✅ | 置顶过的消息永不自动删除；置顶时取消已排程任务，删除前再复查一次 |
| 每群、每类型独立时限 | ✅ | `30s / 5m / 2h / 1d`，5 秒–24 小时；`off` 关闭 |
| 群命令 / 回复私有展示 | ✅ | 优先 ephemeral command + ephemeral 回复；发不出 ephemeral 时降级成群内普通回复，再不行才私聊 |
| 命令 / 回复自动清理 | ✅ | 群里的 `/命令` 立即删除；自然语言命令本体保留；机器人回复默认 20 秒后删除 |
| 停机不丢删除任务 | ✅ | 任务先落 SQLite；启动把 `inflight` 放回 `pending`，逾期立即删 |
| 控制台 `/panel` | ✅ | 私聊：owner 看全部群并退群；群管理员只看自己管理、且机器人在的群，可改这些群的时限/启停/刷新/取消待删 |
| 拥有者全局权限 | ✅ | 任意群（人在群里即可）可直接用 `/autodel`、`/status`，不要求是本群管理员 |
| 群管理员查看/修改本群设置 | ✅ | `/autodel`；群里所有命令前置守卫，非管理员静默丢弃 |
| 实时校验群管理员身份 | ✅ | `Dispatcher::setCommandGuard` + `getChatMember`，失败即拒绝 |
| 机器人权限检查 | ✅ | `/status` + 后台详情：删除消息/限制成员/置顶/邀请/管理群聊 |
| 跟踪机器人进了哪些群 | ✅ | `my_chat_member` + 任意群消息兜底；记录群名、类型、权限快照 |
| 双接收模式 | ✅ | 长轮询 / webhook，共用同一套 Dispatcher |
| 日志 | ✅ | 控制台 + `logs/warden.log`（5MB × 3 轮转）+ `audit_log` 表 |
| 告警 | ✅ | 删除连续失败时私聊 owner，并可同时推送到飞书 / Discord webhook |
| 管理员名册（"我管理的群"） | ✅（缓存） | `/mygroups`；进群/提权时拉取，空名册会自动补拉最近 12 个群 |
| 独立 Web 后台 | ⏳ 待定 | 见第 8 节，需要先确认形态 |
| 有界队列 / worker 池 | ✅ | 队列满时长轮询暂停、webhook 503；按 chat 串行 |
| 更新去重 / 指标 | ⏳ 下一阶段 | 当前单进程、单 io 线程，详见第 6 节 |

## 2. 命令

群菜单默认可以用 ephemeral command（`setMyCommands` 的 `is_ephemeral`），别人
看不到命令消息；但 **`/autodel` / `/status` 故意不用 ephemeral**：ephemeral
command 的 `message_id=0`，机器人删不掉，只有普通命令才能“出现后立即
`deleteMessage`”。`/help` 不在群菜单里，只保留私聊。用户发的 ephemeral command
`message_id` 为 0，机器人无法删除，只能由客户端自己清理。

ephemeral 回复本身也会清理：`sendEphemeralMessage` 返回的
`ephemeral_message_id` 会和 `receiver_user_id` 一起写入 `deletions`
（`message_id` 存负数，避免和真实群消息 ID 冲突），20 秒后由调度器调用
`deleteEphemeralMessage` 删除。`deleteMessage` 不能删 ephemeral 消息，必须用
这个专用方法；删除失败只记 debug，不告警。

如果 ephemeral 不可用，先把回复作为群内普通消息发出，并把它的 `message_id`
排进 `bot_reply` 任务，默认 20 秒后用普通 `deleteMessage` 删除；群内普通消息
也发不出去时，再尝试私聊发送者。三种回复（ephemeral / 群内普通 / 私聊）都会
按 `WARDEN_REPLY_TTL` 清理。

群里的 **本 bot 注册命令** 立即删除：命令守卫会 `deleteMessage`。
未注册命令只有明确写成 `/xxx@本bot` 才删；`/xxx` 和 `/xxx@OtherBot` 都不碰，
避免误删其他 bot 的命令。用户发的 **ephemeral command** `message_id=0`，
Telegram 不允许
bot 删除用户侧副本，所以删不掉；这种命令本来其他成员也看不到，只能由客户端
自己清理。机器人是管理员时普通命令才会真正删掉，不是管理员时删除失败只记日志，
不告警。

自然语言命令 `自动删除 贴纸 开启 10秒` **不删命令本体**，只处理设置变更和回复。
回复会用 Telegram 的 `reply_parameters` 引用触发它的那条消息；斜杠命令因为消息
已经被立即删除，回复不引用。机器人回复默认 20 秒，用 `WARDEN_REPLY_TTL` 配置，
设 0 表示不自动删。

**群里所有命令仅群管理员可用。** Telegram 没有"只允许管理员调用命令"的服务端
开关，Warden 在 `Dispatcher::setCommandGuard` 里实时 `getChatMember` 校验：
非管理员发来的群命令**静默丢弃**——不回复、不改设置、不写审计。私聊命令不受
影响（拥有者后台本来就在私聊）。

| 命令 | 谁可用 | 作用 |
|---|---|---|
| `/autodel` | 群管理员 / 拥有者 | 查看本群开关、各类型时限、我有没有删除权限 |
| `/autodel on` / `off` | 群管理员 / 拥有者 | 开启/关闭；关闭会取消本群所有待删任务 |
| `/autodel sticker 30s` | 群管理员 / 拥有者 | 单类型时限；`off` 关闭该类型 |
| `/autodel all 10m` | 群管理员 / 拥有者 | 所有支持类型统一时限 |
| `/autodel cancel` | 群管理员 / 拥有者 | 只取消待删任务，不改开关 |
| `/autodel reset` | 群管理员 / 拥有者 | 恢复默认并开启（贴纸 60s，图片/视频/语音 5m，其余关） |
| `/status` | 群管理员 / 拥有者 | 刷新并查看机器人在本群的权限清单 |
| `/id` | 群管理员 / 拥有者（群里）/ 任何人（私聊） | 查用户 ID / 群 ID |
| `/panel` | owner / 群管理员（私聊） | owner 看所有群；群管理员看自己管理且机器人在的群，可改时限/启停/刷新/取消待删；退群仅 owner |
| `/mygroups` | 任何人（私聊） | 名册里"我担任管理员"的群 |
| `/start` | 群管理员 / 拥有者（群里）/ 任何人（私聊） | 简短说明 / 私聊入口 |
| `/help` | **仅私聊** | 完整功能说明；群里发会被静默忽略 |

新群默认**开启**自动删除；已有群保留原来的开关状态。之前手动 `/autodel off`
过的群，升级后需要再发一次 `/autodel on`（或 `/autodel reset`）。

### 命令策略表（唯一配置入口）

所有命令的“私聊/群、管理员/owner、菜单、ephemeral”集中在
[commands.hpp](commands.hpp) 的 `kCommandPolicies`，不用再翻各个 handler：

```cpp
struct CommandPolicy {
    std::string_view name;
    std::string_view description;
    bool privateAllowed;   // 私聊是否允许
    bool groupAllowed;     // 群里是否允许
    bool groupAdminOnly;   // 群里是否仅群管理员/owner
    bool ownerOnly;        // 是否仅 WARDEN_OWNERS
    bool groupMenu;        // 是否出现在群管理员菜单
    bool privateMenu;      // 是否出现在私聊菜单
    bool ephemeralCommand; // 群菜单项是否 is_ephemeral
    bool deleteBareCommand; // 没写 @本bot 的注册命令是否也立即删
    GroupDeny groupDeny;   // 群里不允许时：忽略 / 提示去私聊
};
```

业务 handler 仍在 `main.cpp` 注册；策略表只管“谁能用、在哪儿用、菜单怎么显示”。

命令消息删除规则：

```text
注册命令 + @本bot        → 始终立即删
注册命令 + 没 @          → 看该行 deleteBareCommand
未注册命令 + @本bot      → 立即删
未注册命令 + 没 @        → 不处理
@OtherBot                → 不处理
```

`autodel` / `status` 的 `deleteBareCommand` 目前是 `true`；其他命令是 `false`。

> 注：群里"非管理员静默丢弃"是刻意的——如果回"你没权限"，陌生人就能拿
> `/autodel` 探测群设置。私聊里不受限制；`/panel` 会按回调里的群 ID 再做一层
> 鉴权：owner 全部放行，普通用户只能操作 `chat_admins` 名册里自己担任管理员的群。

**群里的本 bot 命令立即删除**：命令守卫会 `deleteMessage`；未注册命令只有
`/xxx@本bot` 才删，`/xxx` 和 `/xxx@OtherBot` 不处理。`/help` 不在群菜单里，
群里发会被静默忽略，只保留私聊 `/help`。
用户发的 ephemeral command `message_id=0`，bot 无法删除，只能由客户端自己清理。
命令删除失败（机器人不是管理员、消息已经不存在）只记日志，不影响命令本身。机器人
的群内回复优先 ephemeral；
ephemeral 不可用时降级为**群内普通回复并自动删除**，普通回复也发不出去才
走私聊兜底。

**菜单也不给非管理员看**：命令菜单分三层作用域发布——

- **default 作用域：空**（避免群成员回落到默认命令）；
- `all_private_chats`：私聊命令（`start/help/panel/mygroups/id`）；
- `all_group_chats`：空，普通群成员在群里打开 `/` 菜单看不到任何命令；
- `chat_administrators`（每个群单独）：该群管理员能看到
  `start/autodel/status/id`（`/help` 只在私聊）。

机器人被拉进群/被提权时会立刻发一次；进程启动后会在后台给已知的、机器人是
管理员的群补发（串行 + 100ms 间隔，避免几百个群同时打 `setMyCommands`）。
每个群多一次调用，换来的是"普通成员连菜单都看不到"。真正的权限仍由
`Dispatcher::setCommandGuard` 兜底，菜单只是体验层。

如果不想承担"每群一次 `setMyCommands`"的调用量，设
`WARDEN_GROUP_MENUS=false`：不再发每群的管理员菜单，群管理员打开 `/` 菜单
看不到命令，但直接输入 `/autodel`、`/status` 仍然可用。私聊菜单不受影响。

> Telegram 的作用域是"没设置就向上回落"的，所以 default 必须是空，
> 私聊命令单独放 `all_private_chats`；只清 `all_group_chats` 不够。

### 权限模型（两级，不做自定义委托）

| 角色 | 范围 | 能做什么 |
|---|---|---|
| **机器人拥有者**（`WARDEN_OWNERS`） | 所有群 | 私聊 `/panel` 看全部群、改任意群的清理时限、启停、刷新权限、取消待删、退群；在任意群（只要人在群里）可直接用 `/autodel`、`/status`，**不需要是本群管理员** |
| **群管理员** | 自己管理的群 | 私聊 `/panel` 只看到自己担任管理员、且机器人在群里的群；可改这些群的时限/启停/刷新/取消待删，不能让我退群 |
| **本群管理员** | 仅自己群 | `/autodel` 查看/修改本群设置、`/status` 看机器人权限；不能碰别的群 |
| 普通成员 | — | 群里命令静默忽略；私聊 `/help`、`/id`、`/mygroups` 可用；`/panel` 只显示空名册提示 |

- 拥有者判定只看 `WARDEN_OWNERS`，不做"谁是群主"的第二套逻辑。
- 不提供"拥有者把某群委托给某人"的自定义多级管理员：群的原生管理员
  就是该群的管理员；再加一层委托只会制造两套真相和撤销不及时的安全问题。
- 拥有者如果不在某个群里，就不能在群里发命令（Telegram 的限制），但可以在
  私聊面板里改这个群的设置。
- 拥有者在群里的命令不要求 `getChatMember` 成功，避免"机器人拥有者不是本群
  管理员"时被误拒。

时限格式：纯数字=秒，`s/m/h/d` 后缀，也支持中文 `秒/分钟/小时/天`；数字和
单位之间可以有空格。范围 5 秒–24 小时（Telegram 只允许删 48 小时内的消息，
这里留余量并避免"设了 3 天却删不掉"的误解）。

也支持中文类型和自然语言（群里直接发，不需要 `/`）：

```text
/autodel 贴纸 20秒
/autodel 图片 开启 1分钟
/autodel 视频 关闭
/autodel 全部 开启 5分钟

自动删除 贴纸 开启 10秒
自动删除 图片 开启 1分钟
自动删除 视频 关闭
自动删除 全部 开启 5分钟
```

类型别名：`sticker/贴纸`、`photo/图片`、`video/视频`、`voice/语音`、
`audio/音频`、`animation/动图`、`video_note/圆视频`、`all/全部`。

`voice` 是 Telegram 的**语音消息**（按住录音的 ogg/opus，带波形和时长）；
`audio` 是作为音乐/音频文件发送的 `audio` 字段（带 performer/title 等）。
Warden 默认删除贴纸/图片/视频/语音，`audio`、`animation`、`video_note` 默认关，
需要时单独 `/autodel audio 5m`。
`开启/打开/启用` 等价于 `on`，`关闭/停用` 等价于 `off`。

删除策略：

- **只处理媒体**：贴纸/图片/视频/语音/音频/动图/圆视频；文本、链接、文件不动。
- **不豁免管理员**：管理员自己发的媒体照删；也不豁免"被回复的消息"。
- **置顶消息跳过**：收到置顶事件时把消息登记进 `pinned_messages` 并取消已排程
  的删除任务；删除前再查一次，防止"排完才置顶"的竞态。
- 保守策略：**一旦置顶过就永不自动删除**（即使后来取消置顶也不补删），避免
  置顶/取消置顶与定时器竞争导致误删。
- **启动补扫**：进程启动时先查出所有"还有未完成删除任务"的群，逐个 `getChat`
  读取 `pinned_message` 并登记/取消任务，**然后才启动删除 worker**。这样
  停机期间被置顶的消息不会因为"逾期任务立刻执行"被误删。
  `WARDEN_PIN_BACKFILL=false` 可关闭，`WARDEN_PIN_BACKFILL_MS` 控制补扫间隔
  （默认 50ms）。
- `/status` 刷新时也会用 `getChat` 补登记；但 `getChat` 只返回当前置顶，
  超级群有多条置顶时，非最新那条依赖运行期间收到的 pin 事件。

## 3. 架构

完整删除流程图（含每个分支删不删）见 [FLOW.md](FLOW.md) / [FLOW.png](FLOW.png)。

```
                    Telegram Bot API
                 ┌─────────┴─────────┐
          long polling            webhook (Beast)
                 └─────────┬─────────┘
                           │ update
                  bounded ingress queue
                  （满了暂停/503）
                           │
                 per-chat lane + N workers
                  （同群串行，不同群并发）
                           │
                    tgbot::Dispatcher
        ┌──────────────────┼────────────────────┐
   my_chat_member       message              callback_query
        │                  │                     │
   群登记/权限快照      媒体识别 + 规则        拥有者面板
        │                  │                     │
        └──────────────┬───┴─────────────────────┘
                       ▼
                 SQLite (WAL)
   chats / chat_settings / deletions / chat_admins / audit_log
                       ▲
                       │ claim / finish / retry
              DeleteScheduler (4 个 worker + 独立节流)
                       │
                       ▼
                 deleteMessage
```

### 代码结构

`main.cpp` 现在只做**组装根**：启动、注册 handler、panel 回调、收发循环。业务按职责拆到：

| 文件 | 职责 |
|---|---|
| `commands.hpp` | 唯一命令策略表：私聊/群/管理员/owner/菜单/ephemeral |
| `warden_types.hpp` | `json` / `net` / `BotClient` 别名 |
| `warden_util.hpp` | ID/名称/命令参数/UTF-8 截断/权限查询等纯工具 |
| `replies.hpp` | `replyEphemeral`、立即删 `/命令`、回复 TTL |
| `warden_menus.hpp` | 群/私聊命令菜单生成与发布 |
| `warden_guard.hpp` | 命令总守卫，按策略表做鉴权 |
| `warden_chat_sync.hpp` | 群记录/权限/管理员名册刷新 |
| `warden_texts.hpp` | `/start`、`/help` 文案 |
| `warden_views.hpp` | 控制台首页/群列表/详情/设置页/键盘 |
| `autodel.hpp` | `/autodel` 解析与执行 |
| `warden_events.hpp` | 媒体排程、`my_chat_member` 事件 |
| `store.*` / `scheduler.*` | SQLite 持久化与删除调度 |

模块当前是 `inline` 头文件，单 TU 编译；需要时可以无痛拆成 `.cpp`。

删除 worker 连续失败时，会经 `AlertDispatcher` 同时投递到 owner 私聊 / 飞书 /
Discord（三者独立，任何一个失败只记日志）。

设计上的几个硬约束：

- **Telegram 没有"列出我加入的群"的 API**。群清单只能靠 `my_chat_member`
  和日常消息积累。所以机器人第一次部署时，历史群不会自动出现，要等一条
  消息、一次管理员操作，或后台刷新。
- **删除任务必须先落库再计时**。`deletions` 以 `(chat_id, message_id)` 为主键，
  重试/重启不会删两次；`state` 只有 `pending/inflight/done/failed` 四态。
- **删除不能用 `BotClient::deleteMessage` 的默认限速**。那个限速是给发消息的
  （单群 1 条/秒），一个活跃群里的贴纸会排到几分钟后。Warden 直接调
  `apiCall("deleteMessage", …, kNoTransportRetry)`，自己按全局 ~30/s、单群
  ~4/s 节流，worker 数可配。
- **群内回复优先 ephemeral**。机器人不是管理员、或客户端不支持 ephemeral 时，
  降级为**群内普通回复**并按 `WARDEN_REPLY_TTL` 删除；普通回复也发不出去才
  走私聊兜底。

## 4. 持久化

一个 `warden.db`（SQLite，WAL）：

| 表 | 内容 | 关键点 |
|---|---|---|
| `chats` | 群 ID、标题、类型、机器人身份、5 项管理员权限、启用开关、加入/离开/最后活跃时间 | 后台/权限面板的数据源 |
| `chat_settings` | `(chat_id, kind) → delay_sec` | 只存覆盖值；没有行时用类型默认值 |
| `deletions` | `(chat_id, message_id)`、类型、到期时间、尝试次数、状态、错误 | 停机恢复的核心 |
| `chat_admins` | 群管理员缓存（含 creator/administrator） | `/mygroups` 用；进群/提权时刷新 |
| `pinned_messages` | 置顶过的消息 | 保守策略：置顶过就永不自动删 |
| `audit_log` | 时间、群、操作者、动作、详情 | 谁在什么时候改了设置/退了群 |

崩溃恢复流程：

1. 启动：`UPDATE deletions SET state='pending' WHERE state='inflight'`
2. worker 领取 `due_at <= now` 的任务；停机期间到期的任务**立即执行**
3. 删除成功 / 消息已不存在 → `done`；无权限 / 400 → `failed` + 告警（每群每小时最多一次）
4. 网络 / 5xx → 指数退避重试，最多 5 次；仍失败 → `failed`
5. `done/failed` 记录保留 3 天；`audit_log` / `pinned_messages` 保留 10 天；
   janitor 启动时先清一次，之后每 6 小时清理
6. SQLite 删除行不会自动缩小文件；文件大小停在历史峰值。需要缩小时停机执行
   `VACUUM`。WAL 由 SQLite 自动 checkpoint，停机时再 `wal_checkpoint(TRUNCATE)`

## 5. 启停与两种接收模式

```bash
# 长轮询（默认，适合本机 / 无公网）
TG_BOT_TOKEN=... WARDEN_OWNERS=123456789 ./warden

# webhook（TLS 由反代终结的形态）
TG_BOT_TOKEN=... WARDEN_OWNERS=123456789 \
TG_WEBHOOK_URL=https://bot.example.com/hook \
TG_WEBHOOK_SECRET=随机串 WEBHOOK_HOST=127.0.0.1 WEBHOOK_PORT=8080 \
./warden
```

- **长轮询**默认 `WARDEN_DROP_PENDING=false`：从 `warden.offset` 续传，停机期间的
  消息重启后还能补排删除任务（Telegram 只保留 24h）。
- **webhook**没有"补收"概念：停机期间 Telegram 的投递会失败/丢弃，重启后不会
  补。需要严格补删就选长轮询。
- 收到 SIGINT/SIGTERM（或 `POST /drain`）后进入 drain：停止接新 update、
  等在途 handler 归零（默认最多 10s，硬超时 +5s），再 checkpoint 退出。
  已落库任务不会丢，下次启动继续；正在删除中的任务会在下次启动被重新领取
  （已经删掉的消息会得到 400，按完成处理）。
- 健康/控制端口默认 `127.0.0.1:9090`：
  ```bash
  curl -fsS http://127.0.0.1:9090/healthz   # 进程活着
  curl -fsS http://127.0.0.1:9090/readyz    # 可以接流量；drain 中 503
  curl -fsS http://127.0.0.1:9090/metrics   # Prometheus 指标
  curl -X POST -H "X-Warden-Token: $TOKEN" \
       http://127.0.0.1:9090/drain          # 优雅停机
  ```

  `WARDEN_CONTROL_TOKEN` 是**本地控制口令**，不是 Telegram bot token：只用于
  `POST /drain` 的 `X-Warden-Token` 头，防止机器上的其他进程/用户随便把机器人
  停掉。留空 = 禁用 `/drain`（返回 403），此时仍可以用 SIGTERM 停机。健康检查
  `GET /healthz`、`GET /readyz` 不需要这个 token；它们默认只监听回环地址，
  不要直接暴露公网。

#### Prometheus 监控

`GET /metrics` 暴露 Prometheus text format。示例配置在
[deploy/monitoring/prometheus.yml](../../deploy/monitoring/prometheus.yml)，
告警规则在 [deploy/monitoring/alerts.yml](../../deploy/monitoring/alerts.yml)。

最常用的曲线：

```promql
# 每秒收到的 update
sum(rate(warden_updates_received_total[1m]))

# 按类型
sum by (type) (rate(warden_updates_received_total[1m]))

# 每秒发出的 Telegram API 请求
sum(rate(warden_telegram_requests_total[1m]))

# 按方法
sum by (method) (rate(warden_telegram_requests_total[1m]))

# 队列饱和度
warden_update_queue_depth / warden_update_queue_capacity

# API 错误率
sum(rate(warden_telegram_errors_total[5m]))
/
sum(rate(warden_telegram_requests_total[5m]))

# 待删任务
warden_deletions_pending

# 进程 RSS
warden_process_resident_memory_bytes
```

指标包括：

- `warden_updates_received_total{type=...}` / `accepted` / `rejected`
- `warden_update_queue_depth`、`_bytes`、`_capacity`
- `warden_worker_busy`、`warden_worker_count`
- `warden_update_wait_seconds`、`warden_update_handle_seconds`
- `warden_telegram_requests_total{method=...}`
- `warden_telegram_request_seconds`、`429`、`errors`、`retries`、`inflight`
- `warden_deletions_pending`、`warden_deletions_finished_total{result=...}`
- `warden_commands_total{command=...}`、`warden_replies_total{channel=...}`
- `warden_process_resident_memory_bytes`、`warden_event_loop_lag_seconds`

指标标签只使用方法名、update 类型、命令名等固定集合，不使用 `chat_id`，避免
Prometheus 基数爆炸。

- 单实例：同一个 token 不能同时跑长轮询和 webhook；SQLite 也不适合多进程
  共享。高负载靠单进程异步 + 限流，不靠多开。

> **部署**：生产建议 webhook + 稳定反向代理 + 蓝绿两槽，旧进程收到 SIGTERM
> 后 drain 再退出。代码侧已提供 `/healthz`、`/readyz`、`POST /drain`；
> 蓝绿切换本身仍需外部脚本或反向代理配合。长轮询只能做到近零停机。

## 6. 高负载：现在做了什么、还差什么

已经做的：

- 全异步单 io_context，handler 不阻塞；有界 ingress + per-chat lane +
  固定 worker 协程，同一群串行、不同群并发，队列满了长轮询暂停 / webhook 503。
- 队列容量和 worker 数可配；lane 空了立即回收，不按群无限增长。
- SQLite WAL + `synchronous=NORMAL`；热路径只读内存缓存（群设置、已登记群），
  每条消息最多一次任务 INSERT；`last_seen` 每群 2 分钟才写一次。
- 删除 worker 数量、全局/单群节流都可配；任务按到期时间走索引。
- 日志分级；逐条消息的排程/删除日志在 `debug`，默认 `info` 不会打爆磁盘。

下一阶段（按优先级）：

1. **更新去重**：webhook 重试可能重复投递同一个 `update_id`；目前删除任务
   幂等，但欢迎/命令类副作用可能重复。方案：内存 LRU + `update_id` 水位。
2. **批量 claim**：4 个 worker 各自 `BEGIN IMMEDIATE` 领 1 条，任务量大时
   事务开销明显；可加 `claimBatch(N)`。
3. **指标**：`/stats` 之外，暴露 Prometheus 文本（队列深度、worker 忙碌数、
   待删任务数、失败数、删除延迟分位、429 次数）。
4. **多线程**：框架的限速器/缓存都是单线程假设，真要吃满多核需要按 chat
   分片到多个 io_context；当前瓶颈更可能在 Telegram 限流而不是 CPU。

### 内存上限

worker 是**协程池**，不是 OS 线程池：网络等待本来就是异步的，多开线程不会更快，
反而要处理 SQLite/限速器/缓存的并发。真正需要多线程时，优先单独加 DB writer。

长期运行时内存由几个硬上限控制：

| 对象 | 上限 / 回收 |
|---|---|
| update ingress | `WARDEN_MAX_PENDING`（默认 2000）和 `WARDEN_MAX_PENDING_MB`（默认 32MB）双上限；满了长轮询暂停 / webhook 503 |
| per-chat lane | lane 空了立即 erase；只保留有积压的 chat |
| worker | `WARDEN_UPDATE_WORKERS`（默认 8）固定数量协程 |
| 群设置缓存 | LRU 4096 个群；淘汰后下次访问从 SQLite 重载 |
| `last_seen` 写合并 | janitor 清 7 天前；超过 20000 条也有硬上限 |
| 删除节流 / 告警节流 | janitor 清 24h / 7 天未活动的群 |
| 命令菜单去重 | 退群 erase；超过 20000 个群清空重发 |
| `knownChats` | 超过 10000 清空，下次消息重新 `ensureChat` |
| 删除任务 | SQLite，不占内存；`done/failed` 保留 3 天 |

所以按消息量增长的内存只有**有界队列**，不会按 update 无限开协程。

### 告警出口（飞书 / Discord）

删除连续失败时，同一条文本会投递到**所有已配置**的渠道；任何渠道失败只写
日志，不影响删除任务：

- Telegram owner 私聊（默认开，`WARDEN_ALERT_OWNER_DM=false` 可关）；
- 飞书自定义机器人 webhook；
- Discord webhook。

```bash
# 飞书：自定义机器人 → 添加 → 复制 webhook；如果开了签名校验，再填 secret
export WARDEN_ALERT_FEISHU_URL="https://open.feishu.cn/open-apis/bot/v2/hook/xxxx"
export WARDEN_ALERT_FEISHU_SECRET="xxxx"     # 没开签名校验就不填

# Discord：频道设置 → 整合 → Webhook → 复制 URL
export WARDEN_ALERT_DISCORD_URL="https://discord.com/api/webhooks/xxx/yyy"

# 自测（owner 私聊，隐藏命令）：
# /alerttest
```

接口在 [alerts.hpp](alerts.hpp) / [alerts.cpp](alerts.cpp)：`AlertDispatcher`
把一条文本分发给多个 `AlertSink`，加新渠道只需要写一个 `makeXxxSink`，
不用改 scheduler。飞书按官方规范带 `timestamp` + `sign`（HMAC-SHA256 +
Base64）；Discord 只发 `content`，超过 1800 字符会按 UTF-8 安全截断。

> 目前的触发点只有"删除连续失败"。同一群每小时最多一条（scheduler 节流），
> 避免权限没配好时刷屏。后续可以在"被踢出群 / 失去删除权限 / 队列堆积"等
> 事件上复用同一个 `publish()`。

## 7. 配置

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `TG_BOT_TOKEN` / 第一个参数 | — | BotFather token |
| `WARDEN_OWNERS` | 空 | 拥有者用户 ID，逗号分隔；**空 = 没有后台** |
| `WARDEN_DB` | `warden.db` | SQLite 路径 |
| `WARDEN_OFFSET_FILE` | `warden.offset` | 长轮询 offset |
| `WARDEN_MODE` | `auto` | `auto/poll/webhook` |
| `WARDEN_DROP_PENDING` | `false` | `true` = 启动丢弃积压（停机即停止）；`false` = 续传 |
| `WARDEN_MAX_PENDING` | `2000` | update 队列条数上限（100–100000）；满了长轮询暂停 / webhook 503 |
| `WARDEN_MAX_PENDING_MB` | `32` | update 队列字节上限（1–512MB），防止大 JSON 撑爆内存 |
| `WARDEN_UPDATE_WORKERS` | `8` | update worker 协程数（1–64） |
| `WARDEN_DELETE_WORKERS` | `4` | 删除并发协程数（1–32） |
| `WARDEN_DELETE_GLOBAL_MS` | `34` | 全局删除最小间隔（≈30/s） |
| `WARDEN_DELETE_CHAT_MS` | `250` | 单群删除最小间隔（≈4/s） |
| `WARDEN_REPLY_TTL` | `20` | 机器人群回复保留秒数；0 = 不自动删 |
| `WARDEN_GROUP_MENUS` | `true` | 每个群发一次 `chat_administrators` 命令菜单；`false` 关闭，命令仍可手动输入 |
| `WARDEN_PIN_BACKFILL` | `true` | 启动时对有未完成任务的群做 `getChat` 置顶补扫 |
| `WARDEN_PIN_BACKFILL_MS` | `50` | 补扫间隔（0–5000ms） |
| `WARDEN_LOG_DIR` | `logs` | 日志目录 |
| `WARDEN_LOG_LEVEL` | `info` | `debug/info/warn/error` |
| `WARDEN_INSTANCE` | 自动生成 | 实例 ID，出现在日志/健康检查里 |
| `WARDEN_HEALTH_HOST` | `127.0.0.1` | 健康/控制端口监听地址 |
| `WARDEN_HEALTH_PORT` | `9090` | 健康/控制端口；0 = 关闭 |
| `WARDEN_CONTROL_TOKEN` | 空 | `POST /drain` 的 `X-Warden-Token`；空 = 禁用控制 |
| `WARDEN_DRAIN_SECONDS` | `10` | 优雅停机等待在途 handler 的上限 |
| `WARDEN_POLL_TIMEOUT` | `10` | 长轮询单次阻塞秒数（5–30） |
| `WARDEN_ALERT_OWNER_DM` | `true` | 删除失败是否私聊 owner |
| `WARDEN_ALERT_FEISHU_URL` | 空 | 飞书自定义机器人 webhook（https） |
| `WARDEN_ALERT_FEISHU_SECRET` | 空 | 飞书签名校验密钥；开了签名才填 |
| `WARDEN_ALERT_DISCORD_URL` | 空 | Discord webhook（https） |
| `TG_WEBHOOK_URL` | 空 | 非空即 webhook 模式 |
| `TG_WEBHOOK_SECRET` | 空 | `X-Telegram-Bot-Api-Secret-Token` 校验 |
| `WEBHOOK_HOST` | `127.0.0.1` | 监听地址；反代形态不要改 |
| `WEBHOOK_PORT` | `8080` | 监听端口 |
| `WEBHOOK_TLS_CERT` / `_KEY` | 空 | 都设置则进程内 HTTPS |
| `TG_WEBHOOK_SKIP_REGISTER` | `false` | 本地自测：不调 setWebhook |

## 8. 需要你拍板的设计问题

这些会显著改变实现，先不动手：

1. **后台形态**：现在是 Telegram 私聊内联键盘（零额外服务、手机可用、身份就是
   Telegram）。要不要独立 Web 后台（表格、搜索、导出、图表）？如果要，认证用
   Telegram Login Widget 还是自建账号？Web 后台和机器人同进程（Beast 另开端口）
   还是独立服务读同一个 SQLite？
2. **权限模型（已定）**：拥有者（`WARDEN_OWNERS`）全局最高权限；群原生管理员
   管理自己群；不做自定义委托管理员。
3. **删除范围（已定）**：只删媒体；管理员自己发的照删；置顶消息跳过；被回复的
   消息不豁免。仍需确认：文本/链接/文件要不要也纳入自动删除？
4. **默认值**：新群默认开启；贴纸 60s、图片/视频/语音 5m。`audio`/`animation`/
   `video_note` 默认关，按需开启。`/help` 和 `/id` 目前
   在群里也按管理员限制；如果希望普通成员能自助看帮助，我可以只对管理类命令
   加守卫。
5. **时限上限**：24 小时够不够？Telegram 硬上限 48 小时，再长只能删个寂寞。
6. **告警**：删除失败现在私聊 owner。要不要 webhook / 邮件 / 群内公告？
7. **数据保留**：`done/failed` 任务保留 3 天、审计日志保留 10 天，是否符合合规预期？
8. **功能边界（已定）**：Warden 当前只做"媒体定时清理 + 群清单/权限后台 +
   告警"；欢迎语、入群验证、防刷屏、关键词过滤、禁言**暂不做**，未来如需
   要再以独立模块接入，不塞进删除调度器，保持单一职责。
9. **部署形态**：单二进制 + SQLite（当前）是否满足？要不要官方 Dockerfile /
   systemd unit / 备份脚本？

## 9. 构建与测试

```bash
# Windows（仓库根目录）
cmd /c build-app.bat
build\Release\warden.exe <token>

# 已有 CMake 构建目录时
cmake --build build --config Release --target warden
cmake --build build --config Release --target warden_tests
build\Release\warden_tests.exe
```

`warden_tests` 不联网，覆盖设置缓存、删除任务的 claim/重试/崩溃恢复、
取消、管理员名册、时限解析和媒体类型识别。CTest 里也能跑：

```bash
ctest --test-dir build -C Release --output-on-failure
```

## 10. 已知限制

- 机器人第一次启动前就在的群，要等一条消息或一次 `/autodel`/`/status` 才会进库。
- 匿名管理员（GroupAnonymousBot）无法 ephemeral、也没有可私聊的真实用户，
  回复会被丢弃；`getChatMember` 对匿名管理员通常查不到，命令会被拒绝。
- ephemeral 消息官方不保证送达（离线/切设备可能丢），关键设置仍会落库并写日志。
- 删除失败告警有每群每小时一次的节流，避免权限没配好时刷屏。
- Telegram 只允许删 48 小时内的消息；任务到期时间超过这个窗口会直接失败。
- `chat_admins` 是缓存：管理员被撤销后，`/mygroups` 可能短暂仍显示该群，
  直到下一次刷新。
- 同一 token 只能跑一个实例；SQLite 也不支持多进程写。

## 11. 非目标与未来扩展点

当前版本明确**不做**（未来可能作为独立模块加，不塞进 Warden 的删除链路）：

- 新成员欢迎语（ephemeral 欢迎、欢迎词模板等）；
- 入群验证 / 验证码 / 人机校验；
- 防刷屏、频率限制、全员禁言；
- 关键词过滤、链接过滤、内容审核；
- 自动封禁 / 警告 / 违规记录。

保留的扩展点：

- 事件入口：`Dispatcher` 的 `onAnyMessage` / `onMyChatMember` / `onCallbackQuery`
  可以挂新模块，不需要改长轮询/webhook；
- 通知出口：`AlertDispatcher` + `AlertSink` 可以复用（欢迎失败、验证失败也能走同一套）；
- 持久层：`Store` 里按新模块加独立表，不要复用 `chat_settings` 的 `kind` 字段；
- 命令守卫：新命令自动继承"群里仅管理员"的规则，不需要重复写权限检查。

原则：**Warden 的核心是"按时间清理媒体"，不是一个全能群管平台。** 未来加功能
时优先做独立示例/独立模块，避免把删除调度器改成一个什么事件都往里塞的上帝对象。

## 12. 上线前检查清单

### 必须做（不做会直接不工作）

- [ ] **BotFather 关闭 privacy mode**：`/setprivacy` → `Disable`。群管机器人要
      看到普通成员发的贴纸/图片/语音；不要赌"管理员身份能绕过"，以实际收到的
      update 为准。改完后如果老群没生效，把机器人移出再拉回。
- [ ] **机器人是群管理员**，至少勾选「删除消息」；否则 `/autodel` 能设置但
      删除会失败并告警。
- [ ] 配置 `WARDEN_OWNERS`（私聊 `/id` 拿 ID），否则没有后台。
- [ ] 在测试群跑一遍：发贴纸/图片/语音 → 到点被删（新群默认开启；
      老群如曾关闭，先 `/autodel on`）。
- [ ] 手动删掉一条已排程消息 → 到点**不告警、不重试**（400 not found 按完成）。
- [ ] 置顶一条已排程媒体 → 到点不删；重启进程，确认启动补扫仍然保护它。
- [ ] `/status` 权限清单正确；`/panel` 能改时限、启停、取消待删。
- [ ] 飞书/Discord 配好后私聊 `/alerttest`，确认两边都收到。
- [ ] 确认运行目录可写：`warden.db`、`warden.offset`、`logs/`。
- [ ] 备份：至少每天 `sqlite3 warden.db ".backup ..."`，并保留 `warden.offset`。

### 正式生产前建议补

- [x] **P0（已实现）**：`GET /healthz`、`GET /readyz`、`POST /drain`、SIGTERM
      优雅 drain、实例 ID、`--version`。drain 会停止接新 update、等在途 handler
      归零、checkpoint 后退出；硬超时 `drainSeconds + 5s`。
- [ ] **P1**：删除任务实例租约（蓝绿/重叠部署时避免两个进程同时跑 worker）。
- [ ] systemd / Docker 的 `Restart=always`、日志轮转、`WorkingDirectory`。
- [ ] 监控：进程存活、`pendingDeletions`、`failedDeletions`、429 日志。
- [ ] 压测/灰度：先 1–3 个群跑一周，再扩到几十个群；SQLite 单机写是上限。

### 更新二进制（当前：优雅重启，不是零停机）

Warden **不会自己重启自己**，它只负责“收到停止信号后把手里的活做完再退出”。
启动新二进制是 systemd / 你的脚本 / 你手动做的事。

手动前台：

```bash
./warden
# Ctrl+C：Warden drain 后退出
# 替换二进制
./warden
```

systemd（推荐）：

```ini
# /etc/systemd/system/warden.service
[Unit]
Description=Warden
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=/opt/warden
EnvironmentFile=/etc/warden.env
ExecStart=/opt/warden/warden
# 崩溃自动拉起；/drain 正常退出码 0，不会被自动拉起（等发布脚本 start）
Restart=on-failure
RestartSec=2
KillSignal=SIGTERM
# 必须大于 WARDEN_DRAIN_SECONDS + 5s，否则 systemd 会提前 SIGKILL
TimeoutStopSec=30

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now warden

# 更新：
sudo systemctl stop warden        # SIGTERM → drain → 退出
sudo cp warden.new /opt/warden/warden
sudo systemctl start warden
# 或者直接 sudo systemctl restart warden（= stop + start，仍有秒级空窗）
```

`/drain` 只是“远程按停止键”，不会帮你启动新进程：

```bash
curl -X POST -H "X-Warden-Token: $TOKEN" http://127.0.0.1:9090/drain
# 进程退出后，仍然需要 systemctl start / 手动启动新二进制
```

**当前没有内置零停机**：stop 到 start 之间有几秒空窗；Telegram 会重试/排队，
已落库的删除任务不会丢。代码侧已有 drain；要真正零停机，需要反向代理 +
蓝绿脚本配合。

### 可以先不做的

- durable inbox（P2，崩溃也不丢 update）；
- `SO_REUSEPORT` / systemd socket activation（P3，无代理零停机）；
- Web 后台、多机 Postgres、欢迎/验证/防刷屏/关键词过滤。

### 当前部署建议

- **第一次上线用长轮询**，变量最少；确认功能后再切 webhook + 反向代理。
- 单实例 + SQLite，先小群灰度；不要一开始就上几百个活跃群。
- 更新二进制目前会有秒级中断（SIGTERM 立即停）；能接受就手动 `systemctl restart`，
  不能接受就上反向代理 + 蓝绿槽，并先用 `/drain` 排空旧进程。


