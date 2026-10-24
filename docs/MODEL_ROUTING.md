# 多模型阶段：按任务选择 Flash／Plus

更新：2026-10-21。状态：实现与隔离工程验收完成；真实 Plus 联通与推荐效果待配置后单独验证。本阶段未调用真实模型，不报告推荐质量提升。

## 作用与必要性

让常规意图识别使用 Flash，复杂音乐规划和研究可使用 `qwen3.7-plus`，在效果、延迟和成本之间提供可测试的分工。模型数量与 Agent 数量分开；本步保留原固定 LangGraph，不增加专业 Agent 或 MCP。

## 你现在怎样配置

打开私有 `backend_py/.env`，保留原 `QWEN_API_KEY`、`QWEN_BASE_URL`、`QWEN_MODEL`。配置模板见 [backend_py/.env.example](../backend_py/.env.example)。不要把 Key 发到聊天、写进客户端或提交 Git。

```dotenv
# 先准备配置，不启用 Plus
QWEN_MODEL_STRATEGY=flash_only
QWEN_PLUS_ENABLED=false
QWEN_PLUS_MODEL=qwen3.7-plus
# 同平台、地域及权限允许时，留空表示沿用原 Key 和接口地址
QWEN_PLUS_API_KEY=
QWEN_PLUS_BASE_URL=
QWEN_PLUS_ENABLE_THINKING=true
QWEN_PLUS_STREAM=true
QWEN_PLUS_JSON_MODE=true
QWEN_PLUS_TIMEOUT_MS=60000
QWEN_PLUS_MAX_TOKENS=8192
```

如需独立凭据，在本机填写 Plus 的 Key 和该平台对应地域的兼容 API base URL，不带 `/chat/completions`。只有规范化后与 Flash 完全相同的 base 才自动复用 Key；不同地址启用时必须显式填 Plus Key，避免把旧密钥隐式带到另一服务。Key 和 endpoint 地域要匹配。若你使用第三方兼容平台，可单独配置地址，不能假定百炼的全部能力均受支持。

完成配置与接口联调后，维护者显式设置 `QWEN_PLUS_ENABLED=true`、`QWEN_MODEL_STRATEGY=task_split`，再重启对应项目后台。两个开关必须同时满足；只填 Key 不会启用 Plus。首次切换可能使旧任务策略不兼容，保留旧预览或新建任务，不静默切换模型继续旧调用。

`/health` 增加 `modelRoutingVersion: 1`。启动器拒绝复用旧版本后台；环境配置修改后仍须正常关闭并重启对应项目进程。此次未自动重启正在运行的用户后台，未更改私有 `.env`。

## 实施步骤与实际流程

```text
读取后台模型配置并验证能力组合
  → 规则／Flash 理解本轮意图
  → 固定 LangGraph 到达模型节点
  → 根据可信任务状态选择 Flash 或 Plus
  → 校验任务策略快照及剩余预算
  → 事务保存所选模型并预留一次调用
  → 唯一一次模型请求
  → 记录实际响应模型、Token 与结果
  → 原有曲库、约束校验、预览和用户确认
```

| 用途 | `task_split` 选择 |
| --- | --- |
| 明确帮助、受支持控制、时长与去重 | 现有程序处理，不新增模型调用 |
| 自由表达的意图路由 | Flash |
| 简单需求理解与研究 | Flash |
| 场景＋新作、动作依赖、续问、多轮修订或多类结构化约束的规划／研究 | Plus |
| 工具观察与候选排序编排 | Flash |
| 既有单步兼容助手／专注指令解析 | Flash |

复杂度来自受校验的路由上下文、已确认 brief、待澄清来源与当前任务状态，不按文字长短或模型自报信心判断。首轮尚无结构化复杂信号时可能先用 Flash，取得有效 brief 后研究阶段再选择 Plus。

`flash_only` 是旧模式对照；`plus_only` 让音乐任务的 plan/research/observe/compose 使用 Plus，但意图路由仍固定 Flash。它不是“所有后台请求都使用 Plus”。这些策略只能由后台配置，用户请求不能传模型名、端点或策略覆盖权限。

## 技术栈与关键边界

使用现有 Python 3.12、FastAPI、HTTPX、LangGraph、MySQL／Redis；SQLite／PG 保留隔离兼容验证，没有新增框架或依赖。

- 模型配置保存思考、JSON、SSE、超时和 Token 上限；Plus 使用有界 SSE，要求完整结束，丢弃推理内容，只解析最终 JSON。Flash 默认仍为非思考、非流式、15 秒。
- 每次最多读取 256 KiB，流事件也有上限；`max_tokens` 包含思考输出。Plus 配置上限为 8192，不代表输出一定正确或必定在时限内完成。
- 调用前在同一额度事务保存 profile、配置模型、指纹、用途、原因及策略版本；结束后记录实际模型与响应 ID。指纹不含 Key，换 Key 不等于更换任务策略。
- 任务固定可用策略快照；未使用的 Plus 预配置不影响 Flash-only 任务。模型、端点身份或参数变更时，继续调用前拒绝悄悄替换。无旧模型记录的历史尝试保持未知。
- 失败、超时与未知调用继续计次数，不自动换模型重发。所有模型共享原账号／全站／任务预算、并发槽位和租约；不会因换模型获得新额度。
- 本步增加 Token 上限与模型审计，**还没有按价格冻结并原子预留的金额预算**。Token 用量未知就保持未知，不能当零费用。更贵模型上线前仍需价格、金额限额和实际效果评测。
- 两张调用表幂等追加可空审计列，不重建用户数据；旧源库迁移只允许这两项已知扩展列缺失，未知差异继续拒绝。实际应用库本轮不作为测试对象。

官方依据：[百炼兼容接口](https://help.aliyun.com/zh/model-studio/compatibility-of-openai-with-dashscope)、[结构化输出](https://help.aliyun.com/zh/model-studio/qwen-structured-output)、[SSE 与用量](https://help.aliyun.com/zh/model-studio/stream)、[思考／流式参数限制](https://help.aliyun.com/zh/model-studio/error-code)。具体型号实际权限与兼容性仍需联调验证。

## 代码在哪里

| 文件 | 作用 |
| --- | --- |
| `backend_py/tango_backend/model_profiles.py` | 两类模型配置、能力限制及公开指纹 |
| `backend_py/tango_backend/model_transport.py` | 唯一 HTTP 适配、有界 JSON／SSE、取消和安全元数据 |
| `backend_py/tango_backend/server.py`、`app.py` | 后台角色装配及不含凭据的策略状态 |
| `backend_py/tango_backend/task_model.py`、`intent_routing.py` | 确定性选型、模型适配和受限结果契约 |
| `backend_py/tango_backend/task_runtime.py`、`conversation_router.py` | 快照校验、调用前选择、额度与恢复守卫 |
| `backend_py/tango_backend/task_store.py`、`mysql_task_store.py`、`postgres_task_store.py`、`conversation_route_store.py` | 三类存储的调用审计与原子预留 |
| `backend_py/tango_backend/store.py`、`migrate_mysql.py` | 审计列升级与旧源迁移兼容 |

## 验证与结果

按 [Harness 规范](HARNESS_ENGINEERING.md) 实际执行，完整命令、输出与源码哈希见 [本阶段记录](agent-upgrade/model-routing-verification.json)：

| 验证 | 结果与范围 |
| --- | --- |
| quick | Python 411 通过、19 跳过；服务／脚本 53 通过；0 失败。跳过为需独立数据库的专项 |
| MySQL／Redis | 919 通过、42 跳过、0 失败；独立临时库，含迁移与实际 Qt HTTP 客户端。42 项为 PG 专属 40 项及 SQLite 不适用 2 项 |
| PG／Redis 兼容 | 897 通过、64 跳过、0 失败；64 项为 MySQL 专属 62 项及 SQLite 不适用 2 项；与 MySQL 重叠，不相加 |
| 原生 macOS Qt | Hosted 6、MusicTask 4 检查通过，其中 6 个业务用例、4 个生命周期；模拟上游和生成静音 WAV |

重点覆盖先配密钥不开启、跨端点凭据隔离、思考／JSON／SSE、取消／超时／截断、业务无效输出保留消耗、用途选择、策略变更与旧任务恢复、Key 轮换、共享额度、三库审计与旧数据迁移。审查修复了脏可选元数据与账本规则不一致、业务校验失败丢失用量、缺失 Token 被补零的问题；开发中测试夹具错误的转录摘要也保留，不冒充完整原始日志。

测试不使用真实 Key、用户库、曲库或付费模型；本轮没有发布、实际应用库升级或云端 CI 执行。全量模型调用用模拟接口，不代表真实模型语义效果，各套数量不相加。

真实效果尚未测试。后续以固定未见任务比较 Flash-only、按任务分工、任务 Plus-only，分别报告终态完成率、硬约束满足、延迟、调用次数和费用；路由保持一致，控制变量。没有这些结果就不能声称省钱或推荐更准确。

## 面试怎么讲

30–60 秒概括：我在已有音乐 Agent 外加了一层后台模型选择策略，简单步骤使用 Flash，复杂规划和研究可使用 Plus。选择由可信任务状态决定，先持久化模型和预留额度再发请求；任务恢复验证原策略，超时不自动切模型重试。HTTP 适配按模型能力处理 JSON 和 SSE，模型输出仍要经过业务校验，工程稳定性与真实模型收益分开评测。

1. **多模型就是多 Agent 吗？** 不是。模型是执行节点的资源，Agent 的状态、工具、分工和循环是另一层设计。
2. **为什么不直接改 model 字符串？** 各型号的思考、流式与输出能力不同，缺乏策略快照和原子账本也无法追踪恢复或消耗。
3. **为什么超时不换更强模型继续试？** 原请求可能已经执行并产生费用；切换也是新尝试，必须有明确策略、剩余预算和记录，本版没有自动付费回退。

下一项主线仍是 [真实 MCP](exec-plans/active/mcp-tools.md)，多模型真实效果需要独立授权和验收。
