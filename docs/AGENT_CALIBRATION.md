# 对话开发校准：验证当前助手是否理解并执行需求

更新：2026-10-23。10 个已公开开发任务、17 条用户输入。评测器工程验证与真实模型成绩分开；这是开发校准，不是独立新题或上线验收。[实际验收记录](agent-upgrade/conversation-calibration-verification.json)。

已执行：专项及运行时 37 通过/15 跳过；快速层 Python 435 通过/19 跳过、Node 53 通过；独立 MySQL 945 通过/43 跳过；离线十题十七轮全通过。均无最终失败，各层重叠不相加。跳过为数据库适用范围，真实模型/曲库请求仍为 0，人工评价待完成；最终原始测试日志与范围见验收记录。

真实执行补充：用户随后授权本轮最多 80 次，已完成一次正式运行，实际 38 次，余 42 次未继续使用。当前模型策略为 Flash-only，Plus 未启用。后台契约 6/10 通过；意图集合可验证匹配 14/17，其中规则 5/5、真实模型路由 9/12。3 轮因提案校验拒绝而没有合法输出，保守保留在分母，不能据此断定原始语义一定错误。真实曲库和人工仍未测。[真实逐题结果与分析](AGENT_CALIBRATION_RESULTS.md)、[真实记录](agent-upgrade/conversation-calibration-live-v1.json)、[独立预算台账](agent-upgrade/conversation-calibration-budget-2026-10-23.json)。上述“0 次”仅指前一阶段离线工程验收，原报告原字节保留。

## 作用、技术与流程

让输入经过真实 Python 后台的消息、意图路由、LangGraph 和持久状态，再核对实际结果。合法 JSON、HTTP 成功、生成预览分别记录，不能相互替代。

技术栈：Python、pytest、HTTPX ASGITransport、生产 FastAPI/LangGraph、隔离 SQLite、JSON 冻结清单、CSV 人工表。ASGI 执行真实应用接口，但没有启动公网服务或操作 Qt。生产仍用 MySQL/Redis，另跑 MySQL 回归，不访问实际用户库。

1. `prepare` 冻结源码、依赖、题集、顺序和预算，创建全新私有目录。
2. 每个任务有自己的合成账号和 SQLite，通过注册、保存消息、路由、音乐任务等实际接口执行。
3. 无界面驱动器模拟客户端派发。合成曲库提前确定，不按模型输出现场编出正确歌曲；实际查询与结果留证。
4. 真实模型沿用已有 HTTP 适配器。所有角色共用发送前持久计数的硬上限，超时、失败、未知都计数，不自动重跑整个任务。
5. 独立核对意图、动作、参数、需求、查询、元数据、时长、锁定、撤回、最终状态和只读恢复。
6. 输出 `report.json`、`progress.json`、`review.html`、`human-review.csv`、请求台账。旧目录禁止续跑；修复后新建冻结运行。

## 十个开发任务

| ID | 步骤 | 检查 |
| --- | --- | --- |
| help | 你能做什么 | 直接帮助，零次模型 |
| scene | 喝酒、20 分钟粤语、不要现场 | 场景、语种、时长、实际查询 |
| releases | 邓紫棋最近 30 天新歌 | 歌手、发行意图、日期、候选证据 |
| compound | 记住粤语喜好，再找微醺歌曲 | 两个意图；模拟拒绝提议时独立找歌继续 |
| negative | 临时听爵士，不要记住 | 不派发记忆变更，保留音乐要求 |
| clarification | 缺歌手 → 补充邓紫棋和时长 | 必要追问，承接近期发行要求 |
| revision | 生成 → 锁第一首后调整 → 撤回 | 保留锁定、未锁定改变、保留旧约束、恢复旧歌单 |
| quoted | 生成 → 转述别人说取消，要求解释 | 不取消、不重生成，解释已有结果 |
| recovery | 生成 → 暂停 → 重开 → 恢复 → 取消 | 只读不调用，恢复已有预览，最终取消 |
| catalog_failure | 注入曲库故障 | 说明失败，不伪装没有匹配歌曲 |

[题集与预写金标](evaluation-data/conversation-calibration-v1.json)在真实请求前冻结。它们已经公开，不能再当独立新题；有限同义词匹配可能误判，争议须人工复核。

## 运行和指标

项目根目录执行：

```bash
backend_py/.venv/bin/python scripts/calibrate-agent.py prepare
# 使用上一步返回的目录
backend_py/.venv/bin/python scripts/calibrate-agent.py run --run .local/calibration-runs/运行目录
```

默认离线，使用明确预写的替身。十题通过也不产生千问准确率。当前按预期分流估算 12 次路由、11 次可能执行任务（含恢复）、每次任务最多 6 次请求，共 78 次。实际错误分流仍受独立 HTTP 硬上限控制。这是次数估计，不是费用报价，不沿用历史 499/500 授权。

以下命令仅在用户明确授权本轮最多 80 次请求后执行：

```bash
backend_py/.venv/bin/python scripts/calibrate-agent.py prepare --mode live --max-provider-requests 80 --authorization 本轮授权引用
backend_py/.venv/bin/python scripts/calibrate-agent.py run --mode live --run .local/calibration-runs/运行目录
```

真实模式仅使用 `backend_py/.env` 的 `QWEN_` 字段，不采用其中的数据库设置或 Cookie，不继承进程模型配置。调用前保存模型策略与请求记录；不足或中断不缩小分母。用量不全保持未知，没有核实单价时费用为 null。

- `backendAcceptanceRate`：10 个后台契约的达标比例，含故障正确停止。
- `intentExactSetRate`：17 轮的允许意图集合匹配率。
- `controlledTaskCompletionRate`、整体 App 完成率、真实曲库召回、听感：仍待充分证据，不由本轮后台契约代替。
- routine、boundary、fault 分组展示。Wilson 区间只描述非随机诊断集，不能推断全部线上用户。
- 源码改变使运行无效；真实模型输出、请求、用量和人工表默认保留在忽略提交的私有目录，不复制进公共报告。

## 你需要做什么

1. 真实执行前明确本轮请求上限。不需要重发密钥；依据 [AGENTS.md](../AGENTS.md)“新增评测预算须明确授权”。
2. 真实运行后打开 `review.html`，核对原话是否完整理解、追问是否必要；填写同目录 `human-review.csv`，前四列保持不变。
3. 理解/交互各填 1–5 分：1 为明显错误，3 为部分可用，5 为完整且合理。结论填 `pass` / `fail` / `uncertain`，再说明原因。未知、空白保留；数值不直接换算准确率。
4. 先重启本机 Python 后台与 App 加载修改，仅重开窗口不一定重启后台。实际 App 另走查 compound、negative、revision、quoted、recovery 五项，核对偏好确认/拒绝、连续修改、播放器和重开体验。合成歌曲没有音频；推荐场景听感必须对真实歌曲另做试听。
5. 导入表格保存新复核记录，不覆盖自动结果。只有一位评审时报告单人评价，不称多人独立盲评：

```bash
backend_py/.venv/bin/python scripts/calibrate-agent.py review --run .local/calibration-runs/运行目录 --review-csv .local/calibration-runs/运行目录/human-review.csv
```

人工表绑定原清单，不能混入别次运行。App 手工调用仍走原账号额度，另记，不混入脚本 80 次预算。

## 发现与修复

原任务完成时阶段为 `done`，暂停后恢复却设置成 `planning`，图在 `done` 直接结束，界面一直显示处理中。现恢复已完成预览直接回到 `preview`，保留歌单、版本检查，不启动模型/工具或自动采用。未完成任务仍走原额度、租约与恢复流程；外层账号认证和禁用保护保留。

代码：[执行与判分](../backend_py/evaluation/calibration.py)、[离线替身/固定曲库](../backend_py/evaluation/scripted.py)、[CLI](../scripts/calibrate-agent.py)、[专项测试](../backend_py/tests/test_calibration.py)、[恢复逻辑](../backend_py/tango_backend/task_runtime.py)。

## 面试说明

30–60 秒：我为音乐助手建立多轮开发题，冻结输入、允许动作和状态。执行器经过真实消息、路由与 LangGraph 接口，使用独立固定候选检查硬约束，付费请求发送前落账并共用上限。自动契约、人工语义和听感分开。校准还发现并修复了已完成歌单暂停恢复卡住的问题。

1. 为什么不用另一个模型全量评分？客观状态由程序检查，主观评价需要人工校准；模型裁判也有偏差和成本。
2. 为什么十题通过不是总体准确率？已公开开发题包含固定曲库和模拟故障，不代表真实用户分布。
3. 如何防止挑最好结果？单次运行锁、源码冻结、预选分母、请求台账、保留失败；修复后新运行。
4. 为什么固定曲库？定位理解/执行问题；真实检索、发行事实及听感必须另测。

下一步完成真实模型与人工复核，修复发现的问题，再进入第 3 步 MCP。最终效果使用新的独立题验证。
