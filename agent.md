# Warden 开发规则

适用于本仓库的所有改动，包括人类开发者和 AI agent。

目标：改动可审查、可回滚、可长期维护，并且不破坏线上数据库。

## 1. 分支与 PR（强制）

- **禁止直接提交到 `main`。**
- 每个新功能、修复、重构、发布准备都从最新 `main` 开分支：
  - `feat/<short-name>`：新功能
  - `fix/<short-name>`：修复
  - `refactor/<short-name>`：重构
  - `docs/<short-name>`：文档
  - `release/<version>`：版本准备
- 流程：
  ```text
  main → 新分支 → 提交 → push → PR → CI + review → merge main
  ```
- PR 未通过 CI、未完成 review 前不要合并。
- PR 描述至少写清：
  - 改了什么
  - 行为变化
  - 数据库是否变化
  - 如何回滚
  - 如何测试
- 不在同一个 PR 里混入无关的大改动。

## 2. 数据库兼容（强制）

线上数据库没有可靠备份，**数据库只能做增量变更**。

### 允许

- `CREATE TABLE IF NOT EXISTS`
- `CREATE INDEX IF NOT EXISTS`
- `ALTER TABLE ... ADD COLUMN ... DEFAULT ...`
- 新增表、新增列、新增索引

### 禁止

- `DROP TABLE`
- `DROP COLUMN`
- 重命名表或列
- 修改已有列的类型或含义
- 删除或重写历史数据
- 需要“先清库再升级”的迁移

### 要求

- 新代码必须能打开旧版本数据库。
- 旧版本二进制必须能打开新版本数据库（回滚兼容）。
- 新增列必须给默认值，旧行读出来要是合法状态。
- 迁移逻辑要可重复执行；重复列/重复表错误要安全忽略。
- 改数据库前先用线上数据库副本做一次启动测试：
  ```text
  复制 warden.db 到临时目录
  → 用新二进制启动
  → 检查 chats / deletions / settings
  → 再用旧二进制启动一次
  ```
- 不允许为了新功能删除旧数据。需要“隐藏”的数据用状态字段表达。
- 每次涉及 DB 的 PR 必须在描述里写“回滚是否安全”。

## 3. 发布与回滚（强制）

- 发布前保留旧二进制：
  ```bash
  cp /opt/warden/warden /opt/warden/warden.<old-version>
  ```
- 数据库变更必须保证旧二进制可运行。
- 部署前备份数据库文件（即使不可靠，也要留一份）：
  ```bash
  sqlite3 /var/lib/warden/warden.db ".backup '/var/backups/warden/warden-$(date +%Y%m%d-%H%M%S).db'"
  ```
- 部署后必须检查：
  - `systemctl status warden`
  - `/healthz`
  - `/metrics`
  - `journalctl -u warden -p warning..err`
  - CPU / RSS / queue depth
- 回滚步骤必须写进 PR 描述：
  ```bash
  systemctl stop warden
  cp /opt/warden/warden.<old-version> /opt/warden/warden
  systemctl start warden
  ```
- 如果新功能风险较高，默认关闭，通过配置或命令显式开启。

## 4. 格式化与测试（强制）

- 所有改动的 C++ 文件必须通过：
  ```bash
  clang-format -i --style=file <changed-files>
  clang-format --dry-run --Werror --style=file <changed-files>
  ```
- 提交前运行：
  ```bash
  cmake --build build --config Release --target warden_tests
  ./build/Release/warden_tests
  ```
- 涉及 worker、队列、停机、删除调度器的改动，必须增加/运行：
  - 队列顺序
  - 背压
  - shutdown drain
  - active job drain
- 涉及数据库的改动，必须增加：
  - 新表/新列读写
  - 旧库迁移
  - 回滚后旧代码可读
- 三平台 CI 必须通过：
  - Linux static
  - macOS arm64
  - Windows x64
- 不允许“本地能跑”代替 CI；CI 失败必须修。

## 5. Review 清单（强制）

每个 PR 合并前至少检查：

### 行为

- 是否保持同群串行、跨群并行？
- 是否破坏队列上限、背压、drain？
- 是否有重复删除、漏删、误删？
- 恢复/关闭功能时是否会补删历史？
- Telegram API 失败是否有退避和上限？

### 数据库

- 是否只做了增量 schema？
- 旧库能否启动？
- 回滚旧二进制能否启动？
- 是否有未提交的事务、WAL 或锁风险？
- 是否有数据无限增长路径？

### 代码

- 是否有明显的空指针、悬垂 `string_view`、生命周期问题？
- 是否在热路径引入不必要的 API 调用或 SQL？
- 错误是否被吞掉？日志是否包含敏感信息？
- 新增函数是否有清晰职责和命名？
- 是否有可测试的纯函数，而不是全部塞进 handler？

### 运维

- 是否有 metrics/日志可观察？
- 是否说明部署和回滚步骤？
- 是否需要新环境变量？是否提供示例，不提交真实密钥？

## 6. 架构与可维护性

- `main.cpp` 只做组装、注册、启动；业务逻辑放独立模块。
- 新功能优先拆成：
  - 纯解析/纯判断函数（可单测）
  - Store/DB 层
  - handler/编排层
- 不要在 handler 里直接拼复杂 SQL；统一走 `Store`。
- 不要在热路径每条消息调用 Telegram API；优先缓存，并说明缓存失效策略。
- 不引入新的无界队列、无界缓存、无界日志。
- 不在单线程 `io_context` 里做阻塞操作。
- 新增后台任务必须有停止/关闭路径，避免 drain 卡住。
- 注释解释“为什么”，不要重复代码字面意思。
- 保持中文注释和用户可见文本一致、可读。
- 新功能应该有清晰的开关和默认值，方便回滚和灰度。

## 7. 安全

- 不提交 token、webhook、密码、私钥、生产数据库。
- 环境变量示例只放占位符。
- 日志不打印完整 token。
- 管理命令必须校验 owner/群管理员。
- 用户输入（用户名、命令参数、文本）不能直接拼 SQL。
- 文件路径、命令参数必须校验，避免注入。

## 8. 本地测试

- 本地测试使用独立测试 bot token，避免和线上 bot 同时 long polling。
- 使用独立数据库：
  ```text
  WARDEN_DB=.../warden-test.db
  WARDEN_OFFSET_FILE=.../warden-test.offset
  WARDEN_HEALTH_PORT=9091
  ```
- 测完后不要提交测试数据库、日志、offset。
- 交付本地测试版本时说明：
  - 二进制路径
  - 版本号
  - 测试 bot 注意事项
  - 数据库兼容性

## 9. 提交信息

使用清晰前缀：

```text
feat(warden): ...
fix(worker): ...
refactor(store): ...
docs: ...
chore(release): ...
```

提交信息说明行为和影响，不只写“update”。

## 10. 紧急修复

紧急修复也必须走分支和 PR。  
如果确实需要先恢复服务，可以临时回滚旧二进制，但代码修复仍要补 PR，不允许把热修直接留在 `main` 而没有 review。
