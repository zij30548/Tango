# 阶段交付文档索引

更新：2026-10-25。用户要求每完成一个阶段，都单独保留交付文档，便于逐步学习、复核和准备面试。下表是已经交付的阶段；它们不因技术迁移或文档整理而被总览替代。

## 已交付阶段

| 阶段 | 单独交付文档 | 实际测试证据 | 完成范围与版本边界 |
| --- | --- | --- | --- |
| 源码开源与隐私隔离 | [开源说明](OPEN_SOURCE_RELEASE.md) | 本次结构检查与 quick：Python 1912 通过／38 跳过，Node 61 通过 | MIT、自有代码与第三方许可分开；独立公开历史不包含旧用户库，原始评测对话留私有原件；源码发布不代表公网产品上线 |
| 第 0 步：基线与验收协议 | [第 0 步交付](AGENT_STEP_0_BASELINE.md) | [阶段记录](agent-upgrade/step0-verification.json) | 历史 Node 基线的工程与契约验收；开发场景未逐题执行，事件契约不等于在线监测已接通 |
| 第 1 步：统一会话与聊天界面 | [第 1 步交付](AGENT_STEP_1_CONVERSATION.md) | [阶段记录](agent-upgrade/step1-verification.json) | 会话、消息回执、任务关联及界面已接入；文档源码入口同步当前版本，阶段测试数字仍属当时 Node 版本 |
| 第 2 步：意图与能力路由 | [第 2 步交付](AGENT_STEP_2_ROUTING.md) | [阶段记录](agent-upgrade/step2-verification.json) | 类型化路由与受控执行的工程验收；该阶段当时尚未独立测评，后续新题运行见本表独立评测行 |
| 内测注册、登录与账号额度 | [账号阶段交付与操作](BETA_ACCESS_AND_QUOTAS.md) | [阶段记录](beta-access-verification.json) | 原交付测试属 Node／PG；当前 Python／MySQL 的操作与保护见交付文档，迁移验证另列 |
| 开发免登录 | [开发模式交付与操作](DEVELOPMENT_MODE.md) | [阶段记录](development-mode-verification.json) | 受保护本机开发身份，保留账号隔离与预算；原交付测试属 Node／PG，不是正式版取消认证 |
| Python 后台迁移 | [Python 迁移交付](PYTHON_BACKEND_MIGRATION.md) | [阶段记录](agent-upgrade/python-migration-verification.json) | Qt 经 HTTP/JSON 与 SSE 接入 Python；当时使用 PG／Redis，兼容性回归不证明推荐质量提升 |
| MySQL 存储迁移 | [MySQL 迁移交付](MYSQL_MIGRATION.md) | [阶段记录](agent-upgrade/mysql-migration-verification.json) | Python 后台切换 MySQL／Redis并保留事务、检查点和额度语义；工程结果不是模型任务完成率 |
| 旧代码清理 | [清理阶段交付](LEGACY_CLEANUP.md) | [阶段记录](agent-upgrade/legacy-cleanup-verification.json) | 移除已被替代的 JS AI 后台与旧工具，保留 Qt、当前 Python 和网易云 Node 适配；没有新增语义效果评测 |
| Harness 开发规范与门禁 | [Harness 阶段规范与交付](HARNESS_ENGINEERING.md) | [阶段记录](agent-upgrade/harness-verification.json) | 五类开发边界及分层工程验收已建立；能力缺口继续登记，CI 仅配置、未云端运行 |
| Flash／Plus 按任务选择 | [多模型阶段交付与配置](MODEL_ROUTING.md) | [阶段记录](agent-upgrade/model-routing-verification.json) | 配置、固定用途选型、模型审计和恢复边界完成隔离工程验收；默认 Plus 关闭，真实联通与质量收益待验证 |
| 第 2 步补充：对话开发校准 | [校准交付与人工操作](AGENT_CALIBRATION.md) | [阶段记录](agent-upgrade/conversation-calibration-verification.json) | 10 个开发任务、17 轮的隔离执行器、判分、请求上限和人工表；真实模型与人工结果单独记录 |
| 第 2 步真实开发校准 | [真实结果与复核](AGENT_CALIBRATION_RESULTS.md) | [真实记录](agent-upgrade/conversation-calibration-live-v1.json) | Flash 实际 38 次；后台契约 6/10，模型意图可验证匹配 9/12；该次是开发题，独立新题后续另列，人工语义仍待复核 |
| 聊天入口与实际失败修复 | [本步交付](AGENT_CHAT_REPAIR.md) | [修复记录](agent-upgrade/chat-repair-verification.json) | 简化页面，补单曲与历史路径、发行隔离、聊天确认；真实模型已知回归保留复合记忆拒绝，不提供总体准确率 |
| 音乐助手演示闭环 | [演示版交付、操作与面试](AGENT_DEMO_RELEASE.md) | [本轮记录](agent-upgrade/demo-release-verification.json) | 受限复合需求编译、聊天锁定与偏好查看；真实Qt／Flash／匿名曲库14次调用验证开发演示，工程与听感分开，不报告总体准确率 |
| 演示版边界复查与授权修复 | [测什么、结果及下一步](AGENT_DEMO_FOLLOWUP_TESTS.md) | [本轮记录](agent-upgrade/demo-followup-verification.json) | 新边界确实发现疑问／截句权限漏洞并修复；补数据库、Qt客户端、登录与图片网络隔离验收，本轮无新增真实模型请求 |
| 只读诊断入口 | [自己查看记录与排查例子](AGENT_DIAGNOSTICS.md) | [验收记录](agent-upgrade/diagnostics-verification.json) | 显式选库，关联会话／路由／任务／工具／检查／账本；MySQL一致只读快照，不在查询中重试模型或修改业务 |
| LangSmith 云观测 | [配置、查看与面试说明](LANGSMITH_TRACING.md) | [阶段证据](agent-upgrade/langsmith-verification.json) | 跨路由／任务／工具回执的受控执行树；真实云端发送与读回单独验收，模拟上游不当作推荐效果 |
| 待确认动作持久恢复 | [本阶段交付、操作与面试](AGENT_PENDING_ACTION_RECOVERY.md) | [阶段证据](agent-upgrade/pending-action-recovery-verification.json) | 原子确认、只读核验、显式续接及未知请求不重发；本机恢复不等于跨设备同步或真实推荐效果 |
| 独立原生端到端新题评测 | [流程、结果、人工复核与面试](AGENT_INDEPENDENT_E2E_EVALUATION.md) | [实测证据](agent-upgrade/independent-e2e-verification.json)、[独立预算](agent-upgrade/independent-e2e-budget-2026-10-24.json) | 24题/40选定交互，真实76次；自动契约7通过/2失败/12证据不足/3阻塞；自由表达与攻击覆盖仍有缺口，人工语义未知，无方法提升对照 |
| 新题问题修复与采用未知恢复 | [流程、代码、测试与面试](AGENT_E2E_REPAIR.md) | [本轮证据](agent-upgrade/agent-e2e-repair-verification.json) | 修复路由、独立曲数、偏好、控制、澄清及原采用ID；固定模型原生10题/15交互/21检查通过，真实模型未复测，原24题成绩不变 |
| 上轮根因复核与残留修复 | [为什么失败、如何修与面试](AGENT_RESIDUAL_REPAIR.md) | [独立证据](agent-upgrade/agent-residual-repair-verification.json) | 纠正上轮覆盖不足，补原只读附句、偏好用途、任务错误/原事实及版本显示；固定模型原生15题/25交互/37检查通过，P03第二轮与P10仍有缺口 |
| 多轮单曲候选与指代 | [流程、代码、验证与面试](AGENT_LOOKUP_CONTEXT.md) | [最终证据](agent-upgrade/lookup-context-followup-verification.json) | 原池/可见顺序、只读refine、来源链/期限/幂等/隔离；固定上游13场景34交互149检查及原15场景回归通过，初12题报告保留，未知声线先澄清，真实语义与曲库另验 |
| 修复后质量验收准备 | [题集、流程、代码、测试与面试](AGENT_REPAIRED_QUALITY_EVALUATION.md) | [离线准备证据](agent-upgrade/repaired-quality-preparation-verification.json) | 16内部新题/8暴露回归、52交互；协议封存、专项169与quick及原生接线通过，0真实请求，等待本轮新预算，不提供模型质量分数 |
| 修复后真实模型首轮验收 | [结果、根因、代码与面试](AGENT_REPAIRED_QUALITY_RESULTS.md) | [实测证据](agent-upgrade/repaired-quality-live-verification.json)、[预算](agent-upgrade/repaired-quality-budget-2026-10-24.json) | 46/350次；新题4/16、回归8/8客观，总业务10通过/12失败/2人工待评；原报告保留，安全误报独立零调用校正，不宣称质量通过 |
| 演示稳定性修复与工程验收 | [流程、代码、测试和演示边界](AGENT_DEMO_STABILITY.md) | [本轮证据](agent-upgrade/demo-stability-verification.json) | 完整单曲/发行/复合需求、只读帮助历史及原任务控制；quick Python1758/28跳过、Node53，固定上游真实Qt核心10任务23交互另1前置、88检查通过；新真实首轮8/60次、18观察，4流程通过/1版本未核/1发行失败，后者只离线修复未复测；不覆盖旧46/350，不报告总体准确率 |
| 聊天工作区与清除闭环 | [功能、流程、代码及面试](AGENT_CHAT_WORKSPACE.md) | [本轮证据](agent-upgrade/chat-workspace-verification.json) | 用户右/助手左有框气泡、推荐单曲点击播放、账号级持久清除；墓碑及原ID跨窗口恢复，偏好/队列/审计/额度保留，零新增真实模型请求；最终工程分数见本阶段，不改旧语义成绩 |

| 音乐需求、Skill与真实曲库核验 | [流程、代码、测试与面试](AGENT_MUSIC_REQUIREMENTS.md) | [工程证据](agent-upgrade/music-intent-verification.json)、[真实证据](agent-upgrade/music-intent-live-verification.json)、[80次独立预算](agent-upgrade/music-intent-budget-2026-10-24.json) | 单曲、作品／歌单路由和统一条件工程已实现；真实7／80次、3／8任务完成、5待同名确认。聊天封面修复及最终门禁通过，确认后真实生成待补；不当总体准确率，动态多Agent/MCP尚未实现 |

| 身份确认后的真实补充诊断与修复 | [结果、根因、代码与面试](AGENT_IDENTITY_FOLLOWUP_RESULTS.md) | [真实诊断证据](agent-upgrade/music-identity-followup-verification.json)、[累计预算](agent-upgrade/music-intent-budget-2026-10-24.json) | 6／40次，确认后作品12首及同任务30→20分钟走通；1题完成1题部分完成，场景0／8和0／5未核验。补可见警告与当前需求同步，未付费重跑，不改原3／8 |

阶段报告保留各自的源码哈希、首次失败、修复、跳过和适用环境。不同阶段、题集和版本的成绩不能相加或改写成当前版本的总体准确率；已经公开的开发题不能作为独立新题。

## 每一阶段都要保留的说明

1. **作用与必要性**：这一步解决什么具体问题，用户行为有什么变化。
2. **实现步骤与流程**：请求怎样进入、执行、确认和结束，失败后怎样处理。
3. **技术栈与取舍**：实际使用什么，为什么采用它，备选方法没有证明什么收益。
4. **代码入口**：主要文件、接口、状态及权限边界，链接到当前源码。
5. **实际测试与证据**：命令、版本、通过／失败／跳过、分母及原始记录；说明是否使用真实模型、曲库或音频。
6. **局限与下一步**：未实现、未验证及待授权的内容，不能把计划写成已完成。
7. **面试问答**：30–60 秒概括、2–4 个追问和回答要点，只讲有证据的实际工作。

[当前 Agent 设计总览](AGENT_DESIGN.md)用于理解现有系统，[开发进度](DEVELOPMENT_PROGRESS.md)用于接续工作；两者都不能替代上面的逐阶段交付文档。后续可整理重复导航，但必须保留阶段说明和原有证据的版本边界。

## 下一阶段

当前按[演示稳定性计划](exec-plans/active/demo-stability.md)完成已暴露问题的代码修复与工程验收，作用与边界见[本阶段](AGENT_DEMO_STABILITY.md)。本轮单独授权60次，单次真实Qt/模型/网易云执行8次、18观察：4流程通过、1指定版本未核、1发行失败；发行附句随后仅离线修复，原真实失败保留、没有重跑取最好值。实际短时采用/播放和窗口重建不能外推整首、会员权益或整进程恢复，固定上游88项工程检查也不代替真实自由表达或听感验收。

此前进入[修复后真实质量验收准备](AGENT_REPAIRED_QUALITY_EVALUATION.md)：16个内部新任务与8个暴露回归、52交互和3前置分别冻结。新入口、专用判分与复核按最终事实判断；离线接线和单元成绩另存，不提供真实质量分数。现已按新350次授权执行46次，详见[首轮结果](AGENT_REPAIRED_QUALITY_RESULTS.md)；新题25%表明质量未达标，余量不自动重测。

待确认动作恢复之后已执行新题评测，并完成本表的业务修复工程验收；本轮新60次已单次执行8次，已知核心流程结果见上段，独立未暴露题及具名人工语义仍待后续验收。攻击触达、旧确认和unknown采用分别保留实际覆盖范围，原24题不重写。之后再做反馈／告警／回归和生产部署验收。新题已暴露，后续不能继续称为未见题。

第 3 步真实 MCP **待实施**，目前只有 [执行计划](exec-plans/active/mcp-tools.md)。完成实际协议发现、工具调用、权限与恢复验证后，再新增单独交付文档和阶段证据，不能把计划链接当成已经实现的证明。
