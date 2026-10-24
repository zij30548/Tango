# Tango 开发导航与硬边界

## 开始工作

用户要求持续遵循 Harness Engineering。先读 [工程规范](docs/HARNESS_ENGINEERING.md)、[能力与缺口](docs/HARNESS_STATUS.json)、[开发进度](docs/DEVELOPMENT_PROGRESS.md)，再按 [升级路线](docs/AGENT_UPGRADE_ROADMAP.md) 和当前[音乐需求计划](docs/exec-plans/active/music-intent-requirements.md)选择一个可验收步骤。前项[演示稳定性计划](docs/exec-plans/active/demo-stability.md)保持原证据。此前[修后质量验收计划](docs/exec-plans/active/repaired-quality-evaluation.md)及首轮失败保留。前项见[多轮候选计划](docs/exec-plans/active/lookup-context.md)，残留复核见[计划](docs/exec-plans/active/agent-residual-repair.md)。前次修复与原验收见[上轮计划](docs/exec-plans/active/agent-e2e-repair.md)。真实MCP的[计划](docs/exec-plans/active/mcp-tools.md)仍待实施。

当前新增[音乐需求计划](docs/exec-plans/active/music-intent-requirements.md)：指定单曲、艺人作品、已有歌单及场景＋歌手＋时长的统一需求；独立交付见[本阶段](docs/AGENT_MUSIC_REQUIREMENTS.md)，新授权80次的真实首轮用7次、3／8任务完成、5待身份确认；其后新授权身份澄清范围最多40次，实际再用6次、两任务1完成1部分完成（歌手与时长达标，散步来源未核验）。[独立账本](docs/agent-upgrade/music-intent-budget-2026-10-24.json)累计13／80，剩余不自动消费，原成绩不改写。前阶段[聊天工作区](docs/AGENT_CHAT_WORKSPACE.md)保留。

本文件只做导航。实现、阶段成绩和失败留在对应文档；代码与当前证据优先，不把计划当成已完成。

开源见 [发布说明](docs/OPEN_SOURCE_RELEASE.md)：MIT、独立历史。私有旧库与原始证据不推送；公开摘要不能冒充冻结原件。禁止镜像推送旧 refs。

用户要求每完成一个阶段单独生成交付说明，入口见 [阶段文档索引](docs/STAGE_DELIVERIES.md)。保留作用、步骤、技术栈、源码、测试及结果、局限和面试问答；架构总览不能替代这些阶段文档，整理旧资料时不得一并合并删除。

## 当前架构

- `backend_py/`：Python 3.12、FastAPI、Python LangGraph；MySQL 8.4/InnoDB 保存账号、会话、任务和额度，Redis 做限流、缓存和通知。
- 根目录 C++/Qt：界面、播放、本机曲库、确认式偏好；SQLite 与网易云授权按探歌账号隔离。
- `service/`：仍需 Node.js 网易云适配。旧 JS AI 后台已清理，不得恢复为默认入口。
- 当前仍是固定音乐研究图与受控路由；真实 MCP、动态 Supervisor、多 Agent 调度及在线质量闭环尚未完成，状态见能力清单。
- 模型可按后台策略分配 Flash／Plus，默认 Plus 关闭；配置、用途、调用审计、恢复及实际验收见 [多模型阶段](docs/MODEL_ROUTING.md)。所有模型共享原额度，Token 上限不等于已实现金额预算，真实效果未验证。

当前流程见 [Agent 设计](docs/AGENT_DESIGN.md)，接入见 [Python 后台](backend_py/README.md)，存储及回滚见 [MySQL 说明](docs/MYSQL_MIGRATION.md)。旧文档合并与归档见 [整理记录](docs/agent-upgrade/document-cleanup-verification.json)；不得把旧哈希和分数覆盖成当前版本成绩。

LangSmith 观测使用后台显式开关和独立密钥，固定摘要经有限队列上传；配置、查看、真实Cloud验收与边界见 [独立阶段](docs/LANGSMITH_TRACING.md)。根记录本轮首次停止点，后续采用等动作追加事实；当前状态仍以MySQL为准。不得恢复自动抓取原始图状态或将私有观测关联传入模型。

工程闭环按用户顺序推进：[待确认动作持久恢复](docs/AGENT_PENDING_ACTION_RECOVERY.md)之后，已执行 [独立新题评测](docs/AGENT_INDEPENDENT_E2E_EVALUATION.md)：76/200次、24题7通过/2失败/12证据不足/3阻塞，原分数保留。随后完成[业务修复工程验收](docs/AGENT_E2E_REPAIR.md)：固定模型原生10题/15交互/21检查通过、0新增真实模型请求；真实模型修后质量仍待复核，不把替身10/10当准确率。后续[残留复核](docs/AGENT_RESIDUAL_REPAIR.md)纠正前次覆盖不足，并追加固定模型15题/25交互/37检查，0真实请求；其后完成[多轮单曲候选](docs/AGENT_LOOKUP_CONTEXT.md)：原池/可见顺序与只读refine，固定上游13场景34交互149检查及原15场景回归通过；未知声线仍追问，P10资料能力待补，真实语义/曲库未复测。再做反馈告警、生产部署及备份恢复验收，MCP与动态Supervisor仍待实施。恢复须核验原路由/原动作事实；已确认未派发要明确继续，unknown只查原ID，不重发、不抢占新任务、不自动采用播放。未用124次与历史余量均不自动用于新评测。

## 五类必须保持的边界

此前[修后真实验收](docs/AGENT_REPAIRED_QUALITY_RESULTS.md)已单次执行46/350次：新题4/16、暴露回归8/8客观达标，业务10通过/12失败/2人工待评；总体语义未知，不能宣称质量通过。保留[原准备](docs/AGENT_REPAIRED_QUALITY_EVALUATION.md)及全部首轮报告；评分器安全误归因已用原观测零请求校正，任务/路由/槽位成绩不变。执行161源/3题文件/二进制原版本已归档核验，后改2评分源码有独立证据；余304次不自动沿用。该轮暴露问题的工程修复见下一段；新版真实效果未验，再后仍需反馈告警及生产验收。

随后完成[演示稳定性代码修复与工程验收](docs/AGENT_DEMO_STABILITY.md)，见[本轮证据](docs/agent-upgrade/demo-stability-verification.json)：完整单曲/版本/只读附句、发行日期与身份限定、复合偏好/歌单、帮助历史及原任务控制，模型提示与HTTP共用Schema且仍过正式权限校验。quick Python1758通过/28跳过、Node53；MySQL/Redis2309通过/63跳过、native6/5/57/27；固定上游真实Qt核心10任务23交互另1前置、88检查通过。两个实际MySQL/Redis后台核实UID/cwd/argv后正常SIGTERM重启并ready200，无清库或额度重置。另按新60次授权单次真实Qt/模型/网易云运行8次，18观察：4流程通过、1指定版本未核、1发行查询失败；发行附句、锁检查及已采用后晚unknown文案随后仅离线修复，未付费重跑；[真实证据](docs/agent-upgrade/demo-stability-live-verification.json)与[独立60次账本](docs/agent-upgrade/demo-stability-budget-2026-10-24.json)保留，余52次不自动消费，原失败不改分。短时播放不证明整首或会员权益，不报告总体准确率，五类能力继续partial；旧46/350及全部首轮成绩保持原版本。

1. 上下文与记忆：当前指令、历史、已确认约束、偏好和工具数据分开；限定输入规模；写记忆须确认。清空偏好不等于删除历史快照。
2. 工具与权限：白名单及明确契约；先核验账号、设备、版本和权限。模型或工具文本不能授予权限；设备提交字段通过结构校验不等于后台认证了上游事实。
3. 执行与状态：超时、调用及补查预算、停止、取消、幂等；MySQL 是权威状态。不能绕过 CAS、owner token/epoch、租约和事务额度；未知付费请求不自动重发。
4. 评测与反馈：先验收与对照；分开工程、语义、真实曲库和听感。保留失败、跳过、分母、版本，不重跑取最好值，不削弱断言凑通过。
5. 可观测性与恢复：新增分支要能关联消息、任务、调用和错误，记录受限脱敏诊断；降级不能把失败变成成功。进度通知不决定任务完成。

## 数据与授权

- 模型密钥只在后台 `backend_py/.env`，日志在 `backend_py/.data/`。Key、Cookie、会话令牌、用户库、私有备份和运行日志不得提交或推送。
- 网易云授权留在设备；不自动上传旧偏好。采用歌单和偏好写入保留明确确认。
- 生产默认内测码注册；普通账号 30 次模型尝试/日、累计 300。失败与未知计数，提额不重置旧消耗。维护必须显式选库，见 [账号管理](docs/BETA_ACCESS_AND_QUOTAS.md)。
- 受保护本机开发身份默认 10000/日、累计 100000，不绕过全站/任务预算、不进生产，见 [开发模式](docs/DEVELOPMENT_MODE.md)。
- 历史真实模型预算 499/500 不构成本轮或后续授权，不自动花完余量；新增评测预算须明确授权，见 [累计账本](docs/agent-evaluation-cumulative-budget.json)。
- MySQL 故障不切回旧 PG/SQLite。旧源库和私有备份保留，迁移须停写、备份与核验，不能直接回退丢失增量。

## 每次改动的闭环

1. 读现状和失败，说明本步作用、必要性、范围及可观察验收。已授权的常规可逆工作继续，不重复申请批准。
2. 实现最小完整改动，检查五类受影响边界。外部调用和架构例外显式说明。新增或修改的自有说明性注释及模块、类、函数文档字符串统一使用中文，解释作用、原因与边界。技术名、标识符和工具指令可保留原文；不改第三方版权、许可证或冻结历史证据。
3. 跑 `python3 scripts/check-harness.py` 和 `python3 scripts/verify-harness.py --tier quick`；存储/并发改动加 `--tier mysql`，界面/播放改动加 `--tier native`。
4. 失败先定位修复再复验，保留原失败；更新清单、进度、设计和独立阶段交付说明，未做的验证明确写未验证。不能因时间或预算结束就标完成。
5. 简明中文交付：作用、流程、技术栈、代码、测试、局限、下一步；附 30–60 秒面试概括和 2–4 个追问要点。

快速门禁不等于数据库、桌面、真实效果或上线验收。CI 未真实运行时只能说已配置。
