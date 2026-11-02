# Warden 热更新设计：零停机替换二进制

目标：启动新版本二进制接替旧版本，Telegram 侧不出现失败、不丢 update、不重复
删除，旧进程把手里的事情做完再退出。

先说结论，避免在错误的方向上雕花：

| 接收模式 | 能否严格零停机 | 原因 |
|---|---|---|
| **webhook + 稳定反向代理 + 蓝绿两槽** | ✅ 可以 | Telegram 只认一个 URL；代理切上游是原子的 |
| **webhook + 双端口 + `setWebhook` 切换** | ✅ 可以（不需要代理） | `setWebhook` 本身是原子操作；端口用 443/8443 |
| **长轮询（getUpdates）** | ❌ 做不到严格零停机 | 同一个 token 同时只能有一个 `getUpdates`，第二个会 409 |
| 进程内 `dlopen` 热加载 `.so` | ⚠️ 不推荐 | C++ ABI / 协程生命周期 / 旧库不能卸载；收益不如蓝绿 |

所以：**生产环境用 webhook**。长轮询只适合本机/无公网，热更新只能做到
"近零停机"（约 1 秒空窗，消息不丢，Telegram 会排队）。

---

## 1. 推荐架构：稳定入口 + 蓝绿两槽

```text
                    Telegram
                       │ HTTPS POST /hook
                       ▼
        ┌──────────────────────────────┐
        │  nginx / Caddy（稳定入口）    │  443，证书只在这里
        │  upstream = blue | green      │
        └──────────────┬───────────────┘
                       │ 127.0.0.1:8080 / 8081
          ┌────────────┴────────────┐
          ▼                         ▼
   ┌─────────────┐           ┌─────────────┐
   │ warden blue │           │ warden green│
   │  旧版本     │           │  新版本     │
   └──────┬──────┘           └──────┬──────┘
          └────────────┬────────────┘
                       ▼
              warden.db（WAL）
        chats/settings/deletions/...
```

关键点：

- Telegram 的 webhook URL 永远指向代理，**部署时不需要改 `setWebhook`**。
- 两个槽位跑同一个二进制、同一个 `warden.db`、同一个 secret；只是监听端口和
  实例 ID 不同。
- 同一时刻只有一个槽位是"流量槽"，另一个是"待命槽"。
- SQLite 只支持同机多进程（WAL），**不支持多机共享**；多机请换 Postgres。

---

## 2. 一次热更新的完整时序

以 blue 正在服务、green 是新版本为例：

```text
t0  blue  127.0.0.1:8080  ← 代理 upstream，正常收 update
t1  启动 green 127.0.0.1:8081（新二进制，registerOnStart=false）
t2  curl http://127.0.0.1:8081/readyz  → 200
t3  改代理 upstream 8080 → 8081，nginx -s reload（或 caddy reload）
       · 新连接立即进 green；已建立的旧连接仍由 blue 处理完
t4  给 blue 发 SIGTERM
       · blue：停止接受新 update（对残余请求回 503）
       · blue：等手里 in-flight handler 结束（硬上限 10s）
       · blue：释放 scheduler lease，checkpoint，退出
t5  green：拿到 scheduler lease，开始执行删除任务
t6  blue 进程消失；完成
```

Telegram 视角：

- t3 之前：全部投递到 blue，正常 200。
- t3 之后：新连接进 green，正常 200。
- t4 期间：极少数还在 blue 上的请求拿到 503，Telegram 自动重试；重试时
  代理已经指向 green，最终成功。
- 全程没有"连接被拒绝"，没有 update 丢失。

回滚同样简单：t5 之前把代理切回 8080 即可；green 还没拿到 lease 就不会
执行删除任务，blue 继续。

---

## 3. 二进制需要具备的能力（实现清单）

**P0 已实现**；P1 仍是蓝绿发布的必要项。

### P0：健康检查 + 优雅退出（已实现）

1. **`GET /healthz`**（默认 `127.0.0.1:9090`）
   - 进程活着、DB 能打开 → 200；drain 中也保持 200（liveness）。
   - body 带 `instance` / `version` / `draining` / `pending_deletions`。
2. **`GET /readyz`**
   - 可以接流量 → 200；drain 中 → 503。
3. **`POST /drain`**
   - 需要 `X-Warden-Token: $WARDEN_CONTROL_TOKEN`；比发信号更适合脚本/Windows。
4. **优雅退出**
   - `SIGTERM` 或 `/drain`：
     1. 置 `draining=true`，`/readyz` 变 503，webhook 停止 accept；
     2. 等待 `Dispatcher` 的 in-flight update 归零（`WARDEN_DRAIN_SECONDS`，
        默认 10s）；
     3. `PRAGMA wal_checkpoint(TRUNCATE)`；
     4. 正常退出码 0；`drainSeconds + 5s` 硬超时强制退出。
   - 当前 scheduler 不等待 worker 把手里的删除做完：任务已落库，下次启动会
     重新领取；这是有意的，避免停机被慢速 API 拖住。
   - **不能**像现在这样信号一到就 `io.stop()`：那会直接砍掉正在处理的 update。
4. **`--version` / 日志里的 instance id**
   - 否则出问题时分不清 8080/8081 哪个是新版。

### P1：删除任务的实例租约（蓝绿重叠时防重复）

现在 `Store::resetInflight()` 在启动时把**所有** `inflight` 放回 `pending`。
蓝绿重叠时，green 一启动就会把 blue 正在删的任务重置，两个进程可能同时删
同一条（虽然 `deleteMessage` 幂等，但会重复告警/重复计数）。

标准做法：

```sql
CREATE TABLE IF NOT EXISTS leases(
  name TEXT PRIMARY KEY,          -- 'delete_worker'
  owner TEXT NOT NULL,            -- 实例 ID
  expires_at INTEGER NOT NULL     -- Unix 秒
);

-- 抢租约：过期或本来就是自己
INSERT INTO leases(name, owner, expires_at) VALUES('delete_worker', ?, ?)
ON CONFLICT(name) DO UPDATE SET owner=excluded.owner, expires_at=excluded.expires_at
WHERE leases.expires_at < ? OR leases.owner = excluded.owner;
```

- 只有租约 owner 启动 `DeleteScheduler` worker；passive 槽只收 update、只落任务。
- 每 30s 心跳续租；优雅退出时 `DELETE FROM leases WHERE owner=?`。
- 进程崩溃后租约最多 60s 过期，新进程接手。
- 拿到租约后再做一次 `resetInflight`（旧 owner 已死或已释放），把崩溃遗留的
  `inflight` 放回 `pending`。
- `finishDeletion` / `retryDeletion` 增加 `AND owner=?` 条件，避免旧进程迟到的
  写覆盖新 owner 的状态。

这样即使 green 在 blue 还没退出时启动，也不会双跑删除 worker。

### P2：ack 前落盘（可选，但推荐）

当前 webhook 是"先回 200，再异步 dispatch"。计划内热更新有 drain 兜底，
但如果旧进程在 200 之后、handler 完成之前崩溃，这条 update 就丢了。

要做到"崩溃也不丢"，标准做法是 **durable inbox**：

```sql
CREATE TABLE IF NOT EXISTS inbox(
  update_id INTEGER PRIMARY KEY,
  received_at INTEGER NOT NULL,
  payload TEXT NOT NULL,
  state TEXT NOT NULL DEFAULT 'pending'   -- pending/done
);
```

webhook 校验通过后先 `INSERT OR IGNORE inbox`，再回 200；后台 worker 按
`update_id` 顺序消费并 `state='done'`。好处：

- ack 与业务解耦，drain 只需要处理 inbox 里剩余的条数；
- 重启/崩溃自动补处理；
- `update_id` 天然去重，webhook 重试不会重复执行命令；
- 后续可以拆成 "gateway 只写 inbox + worker 只消费"。

代价：每条 update 一次 DB 写。以 SQLite WAL 的写入速度，几十到几百 QPS
没问题；真到更高量级再换 Postgres。

P2 不做也能满足"计划内零停机"；做了才满足"任何时刻崩溃都尽量不丢"。

### P3：不用代理的零停机（可选）

Linux 下可以给 webhook 监听加 `SO_REUSEPORT`，让新旧进程短暂共享同一端口：

```cpp
// POSIX only
int one = 1;
setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
```

流程：green 先 bind 同端口（内核把新连接分给它）→ 健康检查 → blue 关闭
listening socket 并 drain。这是 nginx 的经典做法，不需要代理。

前提：

- 仅 Linux（Windows 没有 `SO_REUSEPORT`，`SO_REUSEADDR` 会造成静默抢端口）；
- 两个进程都会收到 update，必须有 P1 的租约 + P2 的 inbox 去重；
- TLS 证书两边都要能读（同一个文件即可）。

Windows 生产环境不建议走这条路；用代理或双端口 `setWebhook`。

---

## 4. 代理配置示例

### nginx

```nginx
# /etc/nginx/conf.d/warden-upstream.conf
upstream warden {
    server 127.0.0.1:8080;   # blue；切 green 时改成 8081
    keepalive 32;
}

server {
    listen 443 ssl http2;
    server_name bot.example.com;

    ssl_certificate     /etc/letsencrypt/live/bot.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/bot.example.com/privkey.pem;

    location = /hook {
        proxy_pass http://warden;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        # Telegram 的 secret token 头必须透传，Warden 会校验
        proxy_set_header X-Telegram-Bot-Api-Secret-Token $http_x_telegram_bot_api_secret_token;
        proxy_read_timeout 30s;
        proxy_connect_timeout 5s;
    }

    location = /healthz { proxy_pass http://warden; }
    location = /readyz  { proxy_pass http://warden; }
}
```

切流：

```bash
sed -i 's/127.0.0.1:8080/127.0.0.1:8081/' /etc/nginx/conf.d/warden-upstream.conf
nginx -t && nginx -s reload
```

`nginx -s reload` 是平滑的：旧 worker 把已有连接处理完，新连接走新配置。

### Caddy

```caddyfile
bot.example.com {
    reverse_proxy 127.0.0.1:8080
}
```

改文件后 `caddy reload --config /etc/caddy/Caddyfile`。Caddy 的 reload 同样
不丢现有连接。

### 没有代理：双端口 + `setWebhook`

Telegram 允许的 webhook 端口只有 **443 / 80 / 88 / 8443**。同一台机器上：

```text
blue  监听 443   URL https://bot.example.com/hook
green 监听 8443  URL https://bot.example.com:8443/hook
```

流程：

1. green 用 `TG_WEBHOOK_URL=https://bot.example.com:8443/hook` 启动，
   `registerOnStart=false`（先别抢流量）；
2. 健康检查 `curl -k https://127.0.0.1:8443/healthz`；
3. 手工 `setWebhook` 到 8443（或让 green 以 `registerOnStart=true` 启动）；
4. 给 blue 发 SIGTERM，drain 后退出；
5. 下次发布反过来：green 在 443，blue 在 8443。

注意点：

- 两个端口都要在防火墙/安全组放行；
- 证书要覆盖带端口的 URL（同一张证书即可）；
- `setWebhook` 调用失败就中止发布，blue 仍是流量槽；
- 没有代理时，`drop_pending_updates` 要显式设 false，避免切换瞬间清积压。

---

## 5. 长轮询的降级方案（做不到零停机）

如果部署环境没有公网 HTTPS、只能用长轮询：

```text
old: 收到 SIGTERM → 停止 getUpdates → 等 in-flight → 保存 offset → 退出
new: 等 old 退出/释放锁 → 启动 getUpdates
```

- 同一个 token 不能并行轮询，409 会一直重试；所以必须有先后。
- 这段空窗通常 0.5–2 秒；Telegram 保留 24 小时 update，**消息不会丢**，
  只是延迟。
- 用 `WARDEN_DROP_PENDING=false` + 持久化 offset，新进程从旧 offset 续传；
  `update_id` 去重可以避免边界重复（P2）。
- 标准实现是用一个 `flock` 独占锁文件：新进程拿不到锁就等待，旧进程退出即
  释放；比"睡 2 秒再启动"可靠。
- 如果要求严格零停机，答案只有一个：**上 webhook**。

---

## 6. Windows 说明

- 运行中的 `.exe` 不能被覆盖，但可以**先改名再放新文件**（`MoveFileEx`）；
  旧进程继续从已映射的映像运行，退出后再删改名后的文件。更稳的做法是让
  安装器在旧进程退出后再替换。
- 没有 `SIGUSR2`；用 Windows 服务（NSSM/WinSW）的 stop/start，或者自己开一个
  本地控制端口/命名事件来触发 drain。
- 没有 `SO_REUSEPORT`；零停机请用"代理"或"双端口 + setWebhook"。
- 开发机是 Windows 不代表生产要 Windows；这个机器人（全静态 musl 单文件）
  更适合放 Linux。

---

## 7. Docker / Kubernetes

- **SQLite + 多 Pod 不行**：`ReadWriteOnce` 卷挂不到两个 Pod；即便挂到同一
  台机器，多机 NFS 上 SQLite 的锁也不可靠。
- 单副本 + 同 Pod 的 sidecar 代理可以做蓝绿，但本质还是单机。
- 要多副本滚动更新，先把存储换成 Postgres（`Store` 已经是独立层，替换后端
  即可），再加分布式租约。
- K8s 的 `strategy: RollingUpdate` 只有在 readiness 正确、且共享存储支持
  多写时才是零停机；否则只是"快速重启"。

---

## 8. 实施顺序与工作量

| 阶段 | 内容 | 工作量 | 产出 |
|---|---|---|---|
| P0 | `/healthz`、`/readyz`、`/drain`、drain、instance id、`--version` | ✅ 已完成 | 可做蓝绿，计划内不丢 |
| P1 | DB 租约、过期恢复、owner 条件写 | 0.5 天 | 重叠期不双跑删除 |
| P2 | durable inbox、`update_id` 去重 | 1–2 天 | 崩溃也不丢，可拆 gateway/worker |
| P3 | `SO_REUSEPORT` / systemd socket activation | 1 天 | 无代理零停机（Linux） |

建议顺序：**P0 → P1 → 上代理跑一次真实发布 → 再决定要不要 P2/P3**。不要一上
来就做 P3，收益最低、坑最多。

---

## 9. 验收清单

一次发布必须验证：

- [ ] 切流前 `readyz` 200，切流后新实例日志出现新 `update_id`。
- [ ] 切换瞬间 `curl -i https://bot.example.com/hook` 不出现连接拒绝；最差 503。
- [ ] 旧实例日志出现 `draining`，且在 10s 内退出码 0。
- [ ] 停机期间发的贴纸/图片，在启动后仍按原时限被删除（`deletions` 表可查）。
- [ ] `SELECT COUNT(*) FROM deletions WHERE state='inflight'` 在稳定后为 0。
- [ ] 同一 `update_id` 不会执行两次（P2 后可用日志/审计验证）。
- [ ] 回滚：切回旧槽，旧槽仍能正常收 update。

---

## 10. 需要你确认

1. 生产是 **Linux + systemd**、**Docker** 还是 **Windows 服务**？
2. 能不能在前面放 **Caddy/nginx**？能的话直接走第 1 节，最简单也最稳。
3. 生产是否必须保留**长轮询**？如果能固定用 webhook，热更新就是真零停机；
   否则只能接受约 1 秒空窗（消息不丢）。
4. 是否接受 **SQLite 单机**这个前提？要上多机/K8s 多副本，得先换 Postgres。

P0 已完成；接下来是 P1（DB 租约）和一套 nginx/systemd 发布脚本。P1 没做之前，
单实例重启是安全的（drain + 任务落库），但不要做两个进程同时消费的蓝绿。

