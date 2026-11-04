# Warden · 群务管家

Warden 是一个 C++20 Telegram 群管理机器人：按群、按媒体类型定时删除消息，
支持管理员/指定用户豁免、持久化删除任务、拥有者后台、Prometheus 指标和
长轮询 / webhook 两种接收模式。

## 功能

- 贴纸、图片、视频、语音默认自动删除；音频、动图、圆视频可单独配置；
- 新群默认开启；旧群保留原开关状态；
- 置顶消息永不自动删除；
- 删除任务先写 SQLite，重启后继续执行；
- 群管理员 / 指定用户可豁免某类媒体；
- 有界 update 队列 + 按聊天串行 + 固定 worker；
- 群命令和回复优先使用 ephemeral（仅操作者可见）；
- 群管理员菜单按群发布，普通成员看不到；
- 可选 Prometheus 指标、飞书 / Discord / owner 私聊告警；
- 入群欢迎已关闭，机器人进群只登记、刷新权限和菜单。

## 命令

群内命令默认仅群管理员或 `WARDEN_OWNERS` 可用；非管理员静默丢弃。

| 命令 | 说明 |
|---|---|
| `/autodel` | 查看本群清理设置 |
| `/autodel on` / `off` | 开启 / 关闭自动清理 |
| `/autodel 贴纸 20秒` | 设置单类型时限 |
| `/autodel all 5m` | 所有支持类型统一时限 |
| `/autodel cancel` | 取消本群待删任务 |
| `/autodel reset` | 恢复默认时限并开启 |
| `/ignore` | 回复某人后使用，见下 |
| `/restore` | 回复某人后使用，见下 |
| `/ignore list` | 查看媒体豁免规则 |
| `/status` | 检查机器人在本群的权限 |
| `/id` | 查看用户 ID / 群 ID |
| `/panel` | owner / 群管理员的私聊后台 |
| `/mygroups` | 我管理的群（缓存名册） |
| `/help` | 私聊查看完整帮助 |

时限范围 5 秒 – 24 小时；支持 `30s`、`5m`、`2h`、`1d` 和中文单位。
`off` / `关闭` 表示不删除该类型。

### 媒体豁免

`/ignore` 和 `/restore` 的目标来自**被回复消息的发送者**，因此是精确 user id，
不依赖 `@username`。

回复某人的消息：

```text
/ignore 贴纸
/ignore 贴纸 图片
/ignore 全部

/restore 贴纸
/restore 贴纸 图片
/restore 全部
```

行为：

- 忽略后，新的同类媒体不生成删除任务；
- 开启忽略时，该发送者已排队的同类任务会取消；
- 恢复只影响之后的新消息，**不会补删忽略期间的历史消息**；
- 已经进入 API 调用中的 inflight 删除任务无法取消，这是极小概率竞态。

管理员整体豁免：

```text
/ignore admin on
/ignore admin off
/ignore admin 贴纸 on
/ignore admin 贴纸 off
```

中文自然语言（群内直接发）：

```text
回复某人：
忽略 贴纸
忽略 贴纸 图片
忽略 全部
恢复 贴纸
恢复 全部

管理员（不需要回复）：
忽略管理员 全部
忽略管理员 贴纸
恢复管理员 全部
恢复管理员 贴纸
```

只发 `忽略` 或 `恢复` 会被当作普通聊天，不执行也不回复。  
所有“全部媒体”都必须显式写 `全部`。

备用入口：

```text
/ignore @alice 贴纸
/restore @alice 贴纸
/ignore 123456789 贴纸
```

`@username` 是用户名软规则：Bot API 不能立即解析 user_id，只能等对方下次
发言时按用户名匹配；回复和数字 ID 才是精确方式。

## 快速开始

```bash
# Windows（仓库根目录）
cmd /c build-app.bat
build\Release\warden.exe <token>

# Linux / macOS
cmake --build build --config Release --target warden
./build/warden <token>
```

最小环境变量：

```bash
export TG_BOT_TOKEN="<BotFather token>"
export WARDEN_OWNERS="<你的 Telegram user id，逗号分隔>"
./build/warden
```

`WARDEN_OWNERS` 为空时没有 owner 后台，私聊 `/panel` 不可用。

本地测试建议使用独立数据库和端口：

```powershell
$env:TG_BOT_TOKEN = "测试 token"
$env:WARDEN_OWNERS = "你的用户ID"
$env:WARDEN_MODE = "poll"
$env:WARDEN_DB = "$PWD\test-warden.db"
$env:WARDEN_OFFSET_FILE = "$PWD\test-warden.offset"
$env:WARDEN_HEALTH_PORT = "9091"

.\build\Release\warden.exe
```

不要把同一个 bot token 同时用于本地和线上长轮询，否则会收到 409 Conflict。

## 配置

| 变量 | 默认 | 说明 |
|---|---|---|
| `TG_BOT_TOKEN` / 第一个参数 | — | BotFather token |
| `WARDEN_OWNERS` | 空 | owner user id，逗号分隔 |
| `WARDEN_MODE` | `auto` | `auto` / `poll` / `webhook` |
| `WARDEN_DB` | `warden.db` | SQLite 路径 |
| `WARDEN_OFFSET_FILE` | `warden.offset` | 长轮询 offset |
| `WARDEN_DROP_PENDING` | `false` | `true` 丢弃停机积压；`false` 续传 |
| `WARDEN_LOG_DIR` | `logs` | 日志目录 |
| `WARDEN_LOG_LEVEL` | `info` | `debug/info/warn/error` |
| `WARDEN_INSTANCE` | 自动 | 实例 ID |
| `WARDEN_HEALTH_HOST` | `127.0.0.1` | health 监听地址 |
| `WARDEN_HEALTH_PORT` | `9090` | health/metrics 端口；`0` 关闭 |
| `WARDEN_CONTROL_TOKEN` | 空 | `POST /drain` 的 token；空 = 禁用 |
| `WARDEN_DRAIN_SECONDS` | `10` | 停机等待在途 handler 秒数 |
| `WARDEN_POLL_TIMEOUT` | `10` | 长轮询超时，5–30 秒 |
| `WARDEN_MAX_PENDING` | `2000` | update 队列条数 |
| `WARDEN_MAX_PENDING_MB` | `32` | update 队列字节上限 |
| `WARDEN_UPDATE_WORKERS` | `8` | update worker 数 |
| `WARDEN_DELETE_WORKERS` | `4` | 删除 worker 数 |
| `WARDEN_DELETE_GLOBAL_MS` | `34` | 全局删除间隔 |
| `WARDEN_DELETE_CHAT_MS` | `250` | 单群删除间隔 |
| `WARDEN_PIN_BACKFILL` | `true` | 启动时补扫置顶 |
| `WARDEN_PIN_BACKFILL_MS` | `50` | 补扫间隔 |
| `WARDEN_REPLY_TTL` | `20` | 机器人回复保留秒数；0 = 不删 |
| `WARDEN_GROUP_MENUS` | `true` | 发布群管理员命令菜单 |
| `WARDEN_ALERT_OWNER_DM` | `true` | 失败时私聊 owner |
| `WARDEN_ALERT_FEISHU_URL` | 空 | 飞书 webhook |
| `WARDEN_ALERT_FEISHU_SECRET` | 空 | 飞书签名密钥 |
| `WARDEN_ALERT_DISCORD_URL` | 空 | Discord webhook |
| `TG_WEBHOOK_URL` | 空 | 非空启用 webhook |
| `TG_WEBHOOK_SECRET` | 空 | Telegram secret token |
| `WEBHOOK_HOST` | `127.0.0.1` | webhook 监听地址 |
| `WEBHOOK_PORT` | `8080` | webhook 端口 |
| `WEBHOOK_TLS_CERT` / `KEY` | 空 | 进程内 TLS |
| `WEBHOOK_MAX_CONNECTIONS` | `40` | 最大连接数 |
| `TG_WEBHOOK_SKIP_REGISTER` | `false` | 本地测试不 setWebhook |

布尔值支持 `true/false`、`1/0`、`yes/no`、`on/off`。完整配置见
[config.hpp](config.hpp)。

## 数据与回滚

Warden 只做**增量 schema**：

```text
chats / chat_settings / deletions / chat_admins
media_exemptions
pinned_messages / audit_log
```

0.2.2 新增：

```text
media_exemptions 表
deletions.sender_id 列（默认 0）
```

因此：

- 新版可以直接打开旧库；
- 回滚旧二进制时，旧代码会忽略新表/新列；
- 不需要恢复数据库；
- 0.2.1 之前生成的删除任务 `sender_id=0`，无法按发送者取消，这是历史任务的限制。

发布前保留旧二进制：

```bash
cp /opt/warden/warden /opt/warden/warden.0.2.1
```

回滚：

```bash
systemctl stop warden
cp /opt/warden/warden.0.2.1 /opt/warden/warden
systemctl start warden
```

## 监控

```bash
curl http://127.0.0.1:9090/healthz
curl http://127.0.0.1:9090/readyz
curl http://127.0.0.1:9090/metrics
```

关键指标：

```text
warden_update_queue_depth
warden_worker_busy
warden_deletions_pending
warden_process_resident_memory_bytes
warden_process_cpu_seconds_total
warden_event_loop_lag_seconds
warden_telegram_429_total
```

Prometheus / Grafana 配置见 [deploy/monitoring](../../deploy/monitoring)。

## 架构

```text
Telegram update
  → 有界队列 + per-chat lane
  → update worker
  → 命令 / 媒体处理
  → 媒体豁免过滤（不生成删除任务）
  → deletions 表
  → 删除调度器（节流 + 重试）
```

关键代码：

```text
main.cpp              组装、注册、启动
warden_events.hpp     媒体入口与 my_chat_member
exemptions.hpp        豁免判断
ignore.hpp            /ignore、/restore 解析与 handler
scheduler.*           持久化删除任务
store.*               SQLite 与设置缓存
health.cpp            health / metrics / drain
```

## 已知限制

- `@username` 不能立即解析成 user id，建议回复或使用数字 ID；
- 管理员名册是缓存，新升职管理员需要刷新后才按管理员豁免；
- ephemeral 接口失败时会降级为群内公开回复，并按 TTL 删除；
- 已经开始的 inflight 删除 API 调用无法中止；
- Telegram 只允许删除 48 小时内的消息，Warden 预留 47 小时余量；
- 生产部署请使用 `deploy/warden/warden.service`，不要用 root 直接跑。

## 部署

仓库提供：

```text
deploy/warden/warden.service
deploy/warden/configure.sh
deploy/monitoring/
```

`configure.sh` 会交互式写入 `/etc/warden/warden.env`，不把 token 放进 shell
history；systemd unit 使用独立的 `warden` 用户和只读根文件系统。

部署前请先阅读 [warden.service](../../deploy/warden/warden.service) 和
[configure.sh](../../deploy/warden/configure.sh) 的注释。
