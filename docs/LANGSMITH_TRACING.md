# LangSmith：查看探歌 Agent 的执行过程

更新：2026-10-24。LangSmith 受控追踪接入、真实云端发送及逐项 API 读回已通过。验收执行真实后台流程，模型和曲库使用固定夹具，不代表真实模型推荐效果或音频播放已经验收。

打开 [LangSmith 控制台](https://smith.langchain.com)，需要登录同一 workspace 的有权限账号。本次演示直达链接见私有 `cloud-readback.json`，位置见阶段证据；个人 workspace、项目及运行 ID 不写入公开文档。本轮没有代用户登录网页或检查界面，Cloud 内容已通过 API 核对。

## 这一步做什么，为什么需要

之前用命令分别查路由、任务与调用账本；现在把一次找歌的关键步骤关联到同一条 trace，用树展示耗时、模型尝试、曲库回执、约束检查和预览。它帮助回答“在哪一步失败、为什么等很久、有没有重复调用”。

Trace 是一次执行记录，span 是其中一步；`thread_id` 将同一会话的多次执行关联起来。MySQL 仍决定业务状态，LangSmith 用于观察，不接管账号、额度、恢复或播放器。

本次采用 **LangSmith Cloud 托管平台＋探歌后台 SDK 接入**，探歌后台继续在原位置运行。不是把整个 App 部署到 LangSmith，也没有在本机安装 LangSmith 平台。平台自部署属于 Enterprise 附加方案，需要独立许可与基础设施。[Cloud 说明](https://docs.langchain.com/langsmith/cloud)、[自部署说明](https://docs.langchain.com/langsmith/self-hosted)

## 第一步：配置账号和 Key

1. 在选定区域注册或登录 LangSmith，打开 Settings → API Keys。
2. 为需要的 workspace 创建 **Tracing key / Tracing Only**，复制到本机私有配置。它可以提交 trace，但不能读取已保存的 trace；后台写入不需要额外授予读取权限，查看时使用已登录且有权限的网页账号。[账号与权限说明](https://docs.langchain.com/langsmith/create-account-api-key)
3. 修改 `backend_py/.env`，不要把 Key 发到聊天、写入公开文档或提交仓库：

```dotenv
# 本项目受控摘要追踪开关
QUIET_LANGSMITH_ENABLED=1
# 禁止 SDK 自动采集整个任务或聊天；手工摘要由上面的开关控制
LANGSMITH_TRACING=false
LANGSMITH_API_KEY=在本机填写你的LangSmith密钥
LANGSMITH_PROJECT=tango-development
LANGSMITH_ENDPOINT=https://api.smith.langchain.com
# workspace-scoped Key 通常可留空；需要指定时填写该 workspace 的 UUID
LANGSMITH_WORKSPACE_ID=
```

这里是 **LangSmith Key**，与千问模型 Key 不同。请从 LangSmith 重新生成并复制完整的新格式 Key，检查是否以 `lsv2_` 开头；前缀不能证明密钥有效。项目增加格式守卫，避免把千问等服务的 Key 误发到观测平台，不打印实际密钥。项目名使用字母、数字、点、下划线或短横线，最长 80 个字符。不要直接复制示例占位值运行。

账号区域、网页和 API 端点必须一致；本项目当前接受下面四组 Cloud 地址，不在端点后加 `/v1`。[官方区域列表](https://docs.langchain.com/langsmith/cloud#regional-storage)

| 区域 | 登录网页 | `LANGSMITH_ENDPOINT` | 演示参数 |
| --- | --- | --- | --- |
| GCP US | `https://smith.langchain.com` | `https://api.smith.langchain.com` | `us` |
| GCP EU | `https://eu.smith.langchain.com` | `https://eu.api.smith.langchain.com` | `eu` |
| GCP APAC | `https://apac.smith.langchain.com` | `https://apac.api.smith.langchain.com` | `apac` |
| AWS US | `https://aws.smith.langchain.com` | `https://aws.api.smith.langchain.com` | `aws-us` |

修改后必须**重启本项目对应的 Python 后台**，再打开 App。只关闭播放器窗口，驻留后台可能仍在运行；启动方式与进程归属说明见 [后台说明](../backend_py/README.md)。不要为重启本项目停止其他后台进程。

当前本机普通后台 `16382` 与开发后台 `16383` 均已启用受控追踪。

## 第二步：先看一条不花模型额度的演示

在仓库根目录运行：

```bash
bash scripts/langsmith-demo.sh
```

终端会打印本次私有目录中的 `trace.html` 和 `report.json` 路径。用浏览器打开打印出来的 HTML，展开执行树即可看到每一步的摘要。默认仅写本地文件，不读取模型 Key，不上传 LangSmith。

演示确实执行 FastAPI、会话路由、LangGraph、任务状态机和约束校验；**模型与曲库响应全部由固定夹具提供，设备生效回执也由模拟设备提交**。它验证追踪接线，不证明真实模型理解、网易云检索、Qt 播放或推荐听感。本次真实模型请求和真实曲库请求均为 **0**。

要把同类隔离演示上传到 US 账号：

```bash
bash scripts/langsmith-demo.sh --upload --region us
```

其他区域替换 `--region` 的值；该参数显式覆盖演示的端点，不能与 Key 所属区域不符。`--upload` 只读取 `.env` 中的观测配置，上传本次夹具执行的受限摘要，不会因此调用千问或真实曲库。

查看本次 `report.json`：

- `traceIds`：实际生成的根 trace UUID。
- `langsmithProject`、`langsmithConsole`：项目名和对应区域的网页入口。
- `uploaded`：本轮发送队列是否处理完，且未记录失败、丢弃或待发送；不是网页展示已人工确认。
- `exportHealth`：检查 `exported`、`failed`、`dropped`、`pending`，不能只看进程退出或 `flush` 是否完成。
- `cloudReadbackVerified`：独立的云端读回状态，脚本本身不会凭发送成功把它改成已验证。
- `fixture=true`、`upstreamMode=scripted`：这是模拟上游演示；示例 Token 也是夹具值。

登录相同区域网页，进入对应 workspace，打开 `tango-development` 项目的 Traces，按报告中的 trace UUID 搜索或筛选，打开详情树；会话关联查看根 metadata 的 `thread_id`，再使用该值查看同一 thread。也可使用私有读回报告中的本次演示链接。

如果只用 Tracing key，API 读回会因权限不足而失败；仍可用网页账号查看。不要为让诊断“全绿”而把应用 Key 升级成管理员 Key。

本次验收使用具备读取权限的维护者凭据，仅查询这一条模拟 trace；运行时只需要写入权限，普通接入者无需为了复现而扩大 Key 权限。所有实际 Key 均留在私有 `.env`，不提交仓库。

### 上传失败怎么查

真实联调按发生顺序保留，后面的成功发送不会覆盖前面的失败：

| 轮次 | 实际结果 | 处理 |
| --- | --- | --- |
| 第一次 | HTTP 403；成功导出 0、失败 10、丢弃 75，凭据未获接受 | 更正 Key 并加入格式守卫；前缀不符不是已证实的唯一根因，不能据此断言是权限或批量接口问题 |
| 第二次 | HTTP 422 拒绝缺失或为 `null` 的 `end_time`；85 个事件逐条 HTTP 发送又出现超时 | 等待状态改为独立 `agent.progress`，后台真正聚合后批量发送 |
| 第三次 | 100 个事件／5 个批次全部接收，失败与丢弃均为 0；API 读回 49 个 run 齐全，但本地根 adopted／版本 16，云端根 preview／版本 14 | 查出同根多次终态更新异步乱序，改为冻结首次停止点、仅发送一次根终态；此轮不能算全语义验收通过 |
| 最终复验 | 96 个事件／4 个 HTTP 批次，失败、丢弃、待发送均为 0；读回 48 个 run，ID、全部 outputs、父级和错误逐项一致，每个节点均有有效起止时间 | 根 preview／版本 14、`checkpoint=preview`；采用子节点 adopted／版本 16；duration satisfied、style unverified；独立结果 `cloudReadbackVerified=true` |

| 诊断信号 | 下一步 |
| --- | --- |
| `TRACE_CONFIGURATION_INVALID` | 核对观测专用开关、新生成的 LangSmith Key、合法项目名、端点及可选 workspace UUID |
| HTTP 401／`TRACE_AUTH_FAILED` | 检查 Key 是否完整、有效、过期或已撤销 |
| HTTP 403／`TRACE_FORBIDDEN` | 检查 Key 确实来自 LangSmith、区域一致、workspace 属于此 Key；不要仅凭 403 判断具体原因 |
| HTTP 422／`TRACE_HTTP_FAILED` | 核对上传信封契约；等待根不能发送缺少合法结束时间的更新，进度应走独立节点 |
| HTTP 429／`TRACE_RATE_LIMITED` | 检查平台限额与发送量，稍后显式执行一次新的演示，不自动重发旧队列 |
| HTTP 5xx／`TRACE_HTTP_FAILED` | 查看 LangSmith 服务状态；保留失败记录，服务恢复后显式重测 |
| `TRACE_NETWORK_FAILED` | 检查到所选区域 API 的网络连通；平台失败不应导致音乐任务被宣布失败或成功 |

健康摘要的 `lastErrorCode` 和 `lastHttpStatus` 只保留固定原因码与数字，不复制服务端正文。重新运行 `--upload` 后检查成功导出数量大于 0、`failed=dropped=pending=0`，再打开云端详情核对状态、版本和约束。`flush=true` 或 ID 齐全都不足以证明完整内容正确。

## 第三步：查看自己在 App 中的一次真实执行

1. 完成配置并重启后台，在聊天框发起一条**新的音乐任务**，例如“推荐20分钟跑步歌曲，不要现场版”。正常 App 执行可能消耗现有模型额度，仍走账号、任务及全站预算。
2. 等待预览；如要验证采用流程，再明确采用，让设备提交生效回执。预览、采用请求和设备生效是三个阶段。
3. 在仓库根目录执行：

```bash
bash scripts/diagnose-agent.sh recent
```

从输出复制实际任务 ID，在 `bash scripts/diagnose-agent.sh task --id` 后面填写这个 ID。`复制会话ID` 或 `真实任务ID` 这样的说明文字不能原样作为 ID 运行。完整用法见 [诊断说明](AGENT_DIAGNOSTICS.md)。

任务输出中的 `observation.traceId` 可用于云端查找。`observation.uploadStatus="unknown"` 的含义是：**本地只知道任务保存了追踪关联，不能据此证明云端接收成功**。旧任务或追踪关闭时创建的任务可能没有 traceId；开启追踪不会自动补发旧记录。

到相同区域、workspace 和项目中打开该 trace，再对照本地任务 ID、revision 和状态。路由完成只表示计划已保存；最终应检查 `preview`、检查项和采用回执，不能仅看某个 span 绿色或 HTTP 200。

## 执行树里的步骤怎么读

首个音乐任务的常见链路如下；缺少的信息会停在澄清，需要补查时会出现额外分段，不保证每次形状相同。

```text
agent.conversation
├─ conversation.route           意图、能力计划与授权校验
├─ agent.progress               本次进度快照，节点结束不表示任务根结束
├─ task.run                     一段后台执行
│  ├─ task.entry
│  ├─ task.understand            需求理解
│  ├─ task.research              研究查询方案
│  └─ task.dispatch              派发设备曲库步骤
├─ task.tool                    派发至设备结果验收的区间
├─ task.run                     收到结果后继续执行
│  ├─ task.observe               检查结果，决定是否补查
│  └─ task.compose               排序并形成预览
├─ task.checks                  程序约束检查摘要
├─ task.adopt                   用户请求采用
└─ task.applied                 收到设备生效回执
```

节点内的 `task.model_call` 关联预算预留后的模型尝试，`model.request` 记录实际传输摘要；它们可能嵌套，不是两次模型请求。`task.checkpoint` 表示业务检查点已提交；等待工具、等待任务衔接等状态查看 `agent.progress.outputs` 的 `status` 和 `root_complete=false`，不能把这个已记录完的进度节点当作整项任务完成。约束保留 `satisfied / unmet / unverified / not_requested`，风格听感不因生成歌单而变成已核验。[LLM 追踪字段说明](https://docs.langchain.com/langsmith/log-llm-trace)

**根固定记录本次执行首次已持久化的停止点**，例如预览、澄清、失败或暂停，仅发送一次根终态。此后采用、锁定、撤回只追加事实节点，因此根 `preview`、子节点 `task.applied=adopted`、MySQL 当前任务 `adopted` 可以同时成立。根耗时截至首次停止点；后续操作看对应子节点时间。当前任务状态始终查 MySQL，提前锁定或撤回造成的停止原因看根的 `checkpoint`。

`task.tool` 是**设备黑盒区间**：后台看到了派发和通过账号、设备、版本、租约校验的回执，没有观测 Qt／Node 内部每次网易云 HTTP 查询；设备报告也不等于后台独立认证了全部上游事实。`adopted` 不证明扬声器已发声或会员歌曲能完整播放。

新的用户修改建立自己的根；`resume`／`select_artist` 的后续根通过 `previous_trace_id` 关联前次执行。部分控制消息的路由与任务动作目前可按会话、任务关联，尚未全部合并到同一根；不能宣称任意多轮操作都有一棵连续、完整的分布式执行树。

## 技术与数据边界

- 技术栈：Python LangSmith SDK、显式 span、ContextVar、后台线程与有限队列；业务仍是 FastAPI＋LangGraph＋MySQL。
- 跨工具回调和重启所需的关联保存于任务私有 `_observation`，首次停止点的 `completed_state_version` 随原 CAS 检查点落库；它表示业务已冻结此版本，不表示 Cloud 已确认接收。新执行根不继承该标记。观测字段从模型 `routingContext` 和客户端公开快照剔除，不加入业务请求体或幂等摘要。
- 关闭 SDK 自动抓取，只上传固定字段的白名单投影：业务关联 ID、固定枚举、计数、耗时、可用的 Token 与检查状态。聊天正文、长期偏好原文、模型提示词、工具原始数据、Key、Cookie、令牌、播放地址和模型内部推理不出站；异常只发送固定原因码。
- 有限队列隔离平台故障。`failed` 是导出失败，`dropped` 是队列满或关闭时丢弃，`pending` 是未处理；`close` 有界等待，超时可能仍有未完成发送。`flush=true` 只表示已处理，不保证上传成功。核验一条完整云端 trace 时还需检查这些计数并实际查看云端内容。
- 云端创建和更新经后台聚合后通过 `POST /runs/batch` 发送固定信封；发送末端再次投影白名单，不携带附件或完整任务。进度使用独立且有完整时间的节点，任务根只在终态更新结束时间。单次批量发送不自动重试、不跟随重定向；这些修复不能反推首次 403 的根因。
- 设 `QUIET_LANGSMITH_ENABLED=0` 并重启即可停止后续上传；原业务账本、诊断和恢复规则继续生效，不删除已上传数据。重启不会重发结果未知的付费模型请求，也不会补发过去丢失的追踪。
- 记录的耗时和 Token 不是准确费用账单，也不是任务完成率或推荐准确率。缺失用量保持未知；金额需价格版本和计费口径，语义效果仍需独立题集与人工校准。

主要源码：[追踪适配](../backend_py/tango_backend/agent_tracing.py)、[路由接线](../backend_py/tango_backend/conversation_router.py)、[任务接线](../backend_py/tango_backend/task_runtime.py)、[模型传输](../backend_py/tango_backend/model_transport.py)、[隔离演示](../backend_py/evaluation/langsmith_demo.py)。

## 做了哪些测试，结果能说明什么

| 范围 | 当前阶段记录 | 能证明什么 |
| --- | --- | --- |
| 追踪适配专项 | 59 项通过 | 固定摘要、嵌套关联、容量、进度节点、批量信封、HTTP／网络故障分类及配置错误时不出站 |
| 路由追踪专项 | 10 项通过；与原路由合跑 77 项通过 | 持久时间重建、账号正文隔离、并发幂等、未知结果、失败清理及跨会话 ID 隔离 |
| 演示＋任务 SQLite 联合专项 | 16 项通过／5 项跳过，其中演示 13、SQLite 任务 3 | 跨回调、恢复、根冻结、单次根终态及完整读回断言；无对应跨库 URL 的用例跳过不算通过 |
| 独立 MySQL 追踪专项 | 2 项通过，已包含在下方完整 MySQL 层 | 实际临时 MySQL 中核对持久观测关联与终态冻结 |
| 最终 Harness quick | Python 652 通过／22 跳过；Node 53 通过 | 隔离工程回归；各专项与完整层有重叠，不相加成准确率 |
| 最终独立 MySQL／Redis 门禁 | 1190 通过／57 跳过 | 数据库、并发和恢复边界；跳过及范围见阶段证据 |
| 真实 Cloud | 最终 96 事件／4 批次／48 run，逐项读回一致 | 已核验接收与存储内容；未代用户登录网页进行 UI 检查 |
| 真实模型／真实曲库／音频与听感 | 本轮真实调用和播放均为 0；未验证这些效果 | 演示采用固定模型与曲库夹具、模拟设备回执 |

初次路由联调的 3 项失败及真实云端失败均保留。SQLite／内存导出专项、MySQL、Cloud 与真实播放分别验收；旧成绩不改写成新版本成绩。最终命令、分母、失败与范围统一见 [本阶段证据](agent-upgrade/langsmith-verification.json)。

## 面试怎么讲

30–60 秒：我给探歌的固定 LangGraph 流程增加了显式追踪，把路由、模型尝试、设备工具回执和约束检查关联起来。跨 HTTP 回调的关联随原 MySQL 检查点持久化，追踪失败不改变预算、幂等和业务状态。真实云端联调发现终态乱序覆盖后，我让根只冻结首次停止点，采用另记子节点。最终 48 个节点的输出、父级和错误均经 API 逐项读回核对。验证使用模拟模型和曲库；Qt 内部每次 HTTP、推荐质量和实际播放仍分别验收。

1. **为什么不用全量自动采集？** 图状态含原文、偏好和工具数据，显式白名单让出站范围可测试。
2. **重启后如何继续关联？** 持久保存根 ID、起始时间和父级顺序，回调先校验原业务权限；追踪字段不提供执行权限。
3. **云端绿了就说明任务成功吗？** 还要对照业务持久终态、约束与设备回执；发送成功、推荐符合需求和实际播放是不同证据。
