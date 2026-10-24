# 内测注册、登录与账号额度

2026-10-20 首次交付；2026-10-21 更新当前 Python＋MySQL 的配置、代码与管理命令。原交付的 Node／PG 工程结果保留在下方历史验收段落；当前尚未部署公网。

开发阶段已另加 [本机自动进入](DEVELOPMENT_MODE.md)：仓库双击启动和 F5 不需登录或内测码，使用独立开发身份；本文描述的正式账号准入和额度仍然保留。

## 现在有什么变化

- 登录、注册和确认密码框都有小眼睛；关闭、隐藏或重开后恢复遮蔽。
- 注册增加确认密码，默认必须提供内测码。注册策略由后台决定，配置加载失败时禁用注册。
- 新注册密码为 15–128 个字符；旧账号仍可登录，成功后旧密码哈希自动升级。
- 默认每账号每日 30 次、累计 300 次 AI 模型尝试。复杂任务可能调用多次模型，一条消息不等于一次额度。失败、超时和结果不确定的已预留尝试也占额度。
- 音乐助手顶部显示今日与内测剩余额度。管理员可按账号调整上限、停用 AI；历史和播放仍可使用。

当前数据库使用 **MySQL 8.4/InnoDB＋Redis**：MySQL 保存账号、内测码、会话与用量，Redis 共享限流。SQLite 保留本机数据和显式兼容测试。数据库品牌不能替代业务权限和原子扣额，迁移与当前工程验证见 [MySQL 迁移说明](MYSQL_MIGRATION.md)。

## 注册与调用怎么走

```text
客户端读取公开注册策略
  → 本地检查确认密码、长度和必填码
  → 后台检查请求频率、字段、密码及内测码
  → 密码计算哈希
  → 同一数据库事务：核销内测码＋创建账号＋写入账号额度
  → 创建登录会话，返回令牌

AI 请求
  → 验证登录与请求频率
  → 校验账号／全站／任务预算
  → 在同一数据库事务中预留本次用量
  → 才调用模型
```

内测码是 192 位随机值，数据库仅保存 SHA-256；默认一次使用、7 天到期，支持禁用、有效次数和对应账号配额。无效码先做轻量预检，真实核销时再次在事务内验证。用户名冲突或账号创建事务失败不消耗内测码。**HTTP 响应超时不代表账号未创建；应先尝试登录，避免重复申请内测码。**

用量继续以现有 `ai_usage` 为权威，累计用量是历史计数之和，没有另一份可漂移的余额。MySQL 用户行锁／SQLite 兼容模式的写事务防并发超花；跨日、重启、调整上限都不清零累计用量。每日按 UTC 00:00（北京时间 08:00）重置。

## 你怎么管理

先重新编译客户端并重启本项目后台。启动器会拒绝缺少 `accountFeaturesVersion: 1` 的旧后台，防止新界面误连旧服务。已有账号可以继续登录；`QUIET_REGISTRATION_MODE` 默认 `invite_only`，也可由维护者设为 `closed` 或显式 `open`。客户端不能选择或绕过这个策略。

下面命令需要在仓库根目录运行。`.local/mysql-v1/storage.env` 是当前 MySQL 私有配置；Python 管理工具只读取你明确选择的数据库配置，不自动寻找秘密文件。旧 Node 管理入口已归档。

生成一个内测码：

```sh
mkdir -p .local/private
uv run --project backend_py --frozen python -m tango_backend.admin invite issue \
  --env-file .local/mysql-v1/storage.env \
  --output .local/private/first.invite-codes.json \
  --expires-days 7 --max-uses 1 --daily-limit 30 --total-limit 300
```

用 VS Code 打开输出文件取出 `code`，再自行发给内测用户。明码只写入新文件，权限 `0600`；不会打印到终端，文件已被 Git 忽略，拒绝覆盖已有文件。需要更多码时换一个新文件名。多次使用的同一码仍有确定次数上限。

查看内测码使用情况、禁用未使用次数：

```sh
uv run --project backend_py --frozen python -m tango_backend.admin invite list --env-file .local/mysql-v1/storage.env
uv run --project backend_py --frozen python -m tango_backend.admin invite disable --env-file .local/mysql-v1/storage.env --id 替换成内测码记录ID
```

查看用户 ID、用量和调整额度：

```sh
uv run --project backend_py --frozen python -m tango_backend.admin account users --env-file .local/mysql-v1/storage.env
uv run --project backend_py --frozen python -m tango_backend.admin account show --env-file .local/mysql-v1/storage.env --user-id 替换成用户ID
uv run --project backend_py --frozen python -m tango_backend.admin account set --env-file .local/mysql-v1/storage.env \
  --user-id 替换成用户ID --daily 30 --total 300 --enabled true
```

`set` 设置新的绝对上限，不清空已用量；`--enabled false` 停用该账号 AI；`--daily 0` 或 `--total 0` 禁止调用，0 不代表无限。后台 `.env` 的 `QUIET_AI_DAILY_LIMIT` 另作为每账号日硬上限，生效值取较小者；要提高日额度，需要同时检查这个硬上限并重启后台。CLI 显示账号策略，客户端显示服务端计算后的有效额度。也可用显式 `--sqlite /绝对路径/已有数据库.sqlite` 管理兼容库，不自动创建替代用户库。

## 具体保护与边界

| 保护 | 当前默认／行为 |
| --- | --- |
| 登录／注册 IP 限流 | 20 次／10 分钟，Redis 跨实例共享 |
| 账号登录尝试 | 10 次／10 分钟；统一错误避免暴露密码或用户是否存在 |
| 普通 HTTP | IP 240 次／分钟，账号 180 次／分钟 |
| 新建会话请求 | 每账号 20 次／日，避免免费写库接口被刷 |
| AI 发起频率 | 新服务器默认每账号 5 次／分钟；同账号执行并发受原有租约限制 |
| 付费调用 | 默认账号 30／日、累计 300；新服务器全站 500／日，原任务预算仍生效 |
| 请求资源 | JSON 类型、字段白名单、长度／体积、HTTP 超时、连接上限与密码计算并发保护 |
| 密码与令牌 | 随机盐、固定版本 scrypt-v2、恒定时间摘要比较；令牌只存摘要 |
| 管理权限 | 仅显式连接数据库的维护者 CLI；没有向普通客户端开放提额或发码写接口 |

已有 `.env` 中明确设置的分钟／全站上限会继续生效，模板不会覆盖你的配置。邀请码附带策略优先，默认环境策略用于无显式策略的存量账号和开放注册账号；不会自动改写已有额度记录。

Redis 故障时，新登录、注册、HTTP 写入和新任务拒绝服务，不切换到内存限流。已登录只读恢复仍可查询 MySQL；已接受的任务可能继续下一阶段，每次调用仍受数据库持久日／总／全站／任务预算约束。不能把它描述为“Redis 一断，所有后续模型调用都停止”。

此次工程保护参考 [OWASP 登录防护](https://cheatsheetseries.owasp.org/cheatsheets/Authentication_Cheat_Sheet.html)和[密码存储建议](https://cheatsheetseries.owasp.org/cheatsheets/Password_Storage_Cheat_Sheet.html#scrypt)。公开运营还需要部署 HTTPS、限制数据库网络访问，并在边缘处理大流量攻击；应用限流无法保证抵御所有 DDoS。次数额度也不等于供应商金额账单。邮箱验证、密码找回、MFA 与可视化管理后台本轮未实现。

## 首次交付的验证（历史 Node／PG 版本）

以下数字描述 2026-10-20 的原版本，不替换为当前 Python 版本成绩。当前验证见 [MySQL 迁移说明](MYSQL_MIGRATION.md)，两组数字不相加。

- 最终完整后台与真实临时 PG／Redis回归 **574/574，0 失败、0 跳过**；包含邀请码并发核销、注册回滚、请求伪造、配额竞争、跨日／重启、迁移和故障。
- 5 套 Qt 目标 **53 条功能数据行通过**（含初始化／清理为 63 项），涉及密码眼睛、注册策略、确认密码、额度、账号隔离、完整找歌与本机静音 WAV 播放。
- 20 个并发请求竞争 3 次总额度，只有 3 个成功；失败和不确定调用不退款，旧单步与 Agent 任务共享预算。
- 升级后的 SQLite→PG 迁移保存邀请码、消费审计、账号策略和会话回执；未知表列仍拒绝，幂等与变更后拒绝回滚仍通过。
- 全程使用临时库、模拟模型和曲库；未读取真实 `.env`、调用真实模型、处理真实会员凭据或做公网渗透／负载认证。首次整合失败与后续修复保留在[验收记录](beta-access-verification.json)。

当前核心代码：[密码眼睛](../passwordvisibility.h)、[注册策略](../backend_py/tango_backend/registration_service.py)、[内测码](../backend_py/tango_backend/invite_store.py)、[账号额度](../backend_py/tango_backend/account_quota.py)、[HTTP 保护](../backend_py/tango_backend/app.py)、[管理 CLI](../backend_py/tango_backend/admin.py)。

面试可以这样讲：我把注册改成服务端控制的内测准入，内测码与账号创建原子提交；付费调用前在原事务里预留日额度和累计额度，用用户行锁防并发超花。Redis 负责短期限流，数据库负责持久预算，重启和缓存故障不会重置花费。

- 为什么不靠前端限制？请求可以绕过界面，额度与准入必须由后台检查。
- 为什么不能先查余额再扣？并发请求会同时看到剩余额度；检查与预留必须在同一锁事务中。
- 为什么已有 Redis 还要 MySQL？限流窗口、缓存和持久用量生命周期不同，缓存失效不能恢复已花掉的额度。
