# 自己查看 Agent 记录与定位问题

这一步增加只读维护命令，把已有会话、路由、任务、工具与调用账本汇总到终端。它不会找歌、恢复任务、确认口味、采用歌单或调用模型，也不会建表或修改记录。它是本机维护者入口，未新增面向普通用户的后台页面。

需要网页执行树时，使用 [LangSmith 接入](LANGSMITH_TRACING.md)。开启追踪后创建的任务，查询输出另有 `observation.traceId`，可在对应项目中定位；`uploadStatus=unknown` 只表示本地诊断不能证明云端接收成功。历史任务不会自动补传。

## 先这样操作

先打开 App，让当前 MySQL 服务正常运行。然后在 VS Code 打开项目，选择“终端 → 新建终端”，确认位于项目根目录。

**1．列出最近记录，取得 ID：**

```bash
bash scripts/diagnose-agent.sh recent
```

“关联会话”里的 `id` 是会话 ID；“关联任务”里的 `id` 是任务 ID。列表按更新时间倒序，通常第一条就是刚操作的记录；`updatedAt`是毫秒时间戳，报告快照时间带 UTC 时区。列表默认10条，可加 `--limit 30`。这个入口固定读取本项目 `.local/mysql-v1/storage.env`，不用在命令行输入数据库密码或模型密钥。

**2．查看一段聊天的路由，替换下面的 ID：**

```bash
bash scripts/diagnose-agent.sh conversation --id 复制会话ID
```

查看 `messages` 的消息 ID／顺序、`routes` 的意图和动作、`tasks` 的明确关联任务。默认不显示聊天原文、记忆正文或完整模型提案；需要按你在 App 中发送的顺序和时间对应消息。`messageId` 可用于继续查拒绝日志。

**3．查看具体任务的进度、工具、约束及调用账本：**

```bash
bash scripts/diagnose-agent.sh task --id 复制任务ID
```

如果要检查检索词，明确增加选项：

```bash
bash scripts/diagnose-agent.sh task --id 复制任务ID --show-queries
```

这时会展开有界的本机曲库检索文本；仍不展开聊天、完整工具结果、Cookie或播放地址。只在本机排查需要时使用，不把含个人信息的输出提交仓库或直接公开。

想看适合展开阅读的 JSON，在任何命令末尾加 `--json`。命令不会自动启动数据库，连接失败时先检查 App 是否正常启动。

## 看哪些字段

| 记录 | 重点字段 | 怎样理解 |
| --- | --- | --- |
| 路由 | `planner`、`intents`、`actions`、`targetMinutes`、`effectiveStatus`、`errorCode` | 规则还是模型分流、选了什么能力；`completed`只表示计划通过 |
| 任务 | `status`、`phase`、`revision`、`stateVersion`、`pendingSteps` | 当前做到哪一步，是否等设备查曲库；版本是否递增 |
| 执行轨迹 | `trace`的`node/status/revision/at` | 按时间看首次偏离或停住的阶段；最多保留64条，不是完整永久审计 |
| 工具历史 | `skill`、`status`、`trackCount`、`sourceCount`、`errorCode` | 是接口失败、没有候选，还是已经取回足够候选；显式展开后可检查检索词 |
| 约束检查 | `checks`的`key/status` | `duration`时长、`locks`锁定、`exclude_live`现场、`releases`发行、`style`风格 |
| 调用账本 | `calls`的`node/status/revision/runId`、Token、模型 | 哪次规划／研究／编排失败，是否跨版本；`null`表示未知，不是0 |

约束状态分别读：`satisfied`＝该项程序检查满足；`unmet`＝未满足；`unverified`＝证据不足或当前能力未核验；`not_requested`＝本次没要求。风格和听感经常是 `unverified`，不能擅自计成失败或通过。

规则路由也有尝试记录，但 `paid=false`，不能把所有尝试行当成模型调用。`reserved`是调用前预留，不能证明HTTP已发；`uncertain`表示结果不确定。当前任务的调用列表也不是账号总用量，账号额度仍通过已有管理命令查询。

## 三个排查例子

**例一：发送后提示“未形成可执行方案”。**

先查会话。如果对应路由是 `failed + INVALID_ROUTE_PLAN`，说明未通过计划校验，应先查细分拒绝原因，不必先改曲库搜索。终端执行：

```bash
rg 'route_plan_rejected' backend_py/.data/backend.log
```

按刚才的 `messageId`／路由 `id` 对应日志。`memory_source_permission`表示记忆提议缺少有效授权；`dependency_order`表示动作依赖不合法。细分原因目前只在私有日志中，命令不会假装数据库已有该字段。没有匹配日志不能推定是哪一种原因。

此前“记住我喜欢粤语吗？”被模型截掉问号的缺陷，就是先以测试复现，再修 [授权校验](../backend_py/tango_backend/intent_routing.py)，随后验证持久失败与不重复调用；见 [修复记录](AGENT_DEMO_FOLLOWUP_TESTS.md)。

**例二：歌单不符合场景。**

先检查路由和任务的时长／排除要求。如果任务要求正确，再看工具：候选数为0或有错误码，查检索词、接口与授权；候选已返回但选歌不合适，再查排序及来源证据。如果各项硬约束满足、只有 `style=unverified`，表示尚未证明听感符合场景，需要试听反馈，不能靠“模型调用成功”判断推荐正确。

**例三：改成40分钟却还是旧歌单。**

检查修改消息是否产生 `music.task` 的 `mode=continue`，关联的 `taskId` 是否一致；再查该任务的 `revision`是否增加、`targetMinutes`是否变40、`status`是否重新进入规划或预览。若后台已有40分钟新预览而App仍显示旧结果，定位Qt会话／版本同步；若路由完成但新任务没有持久证据，排查设备执行及同步，不能猜测已经执行。

注意：本机查看口味、确认、单曲搜索等操作不一定建立研究任务。会话未同步任务关联、查询列表被截断，也不能直接判断任务不存在；报告会注明这些边界。采用状态表示收到设备回执，整首可播放和听感还需实际客户端验证。

本次也读取了之前隔离桌面演示的真实持久记录：该任务已采用，当前目标20分钟，6首、1200003毫秒；两次曲库研究各返回40首候选、2个来源；调用账本6行，轨迹保留20→40→撤回→暂停→恢复→采用。`duration=satisfied`而`style=unverified`，表示时长已由程序核验，听感仍待试听。这里只读取原证据，没有重跑模型。

## 代码、技术与验收

- [诊断CLI](../backend_py/tango_backend/diagnostics.py)：Python／argparse，显式选库、固定查询和白名单输出。
- [本机快捷命令](../scripts/diagnose-agent.sh)：固定选择当前MySQL配置，不加载模型配置或自动安装依赖。
- SQLite兼容验收使用 `mode=ro/query_only`；MySQL使用只读一致快照，报告期间并发写入不会混成另一版本。
- [SQLite与安全测试](../backend_py/tests/test_diagnostics.py)、[MySQL专项](../backend_py/tests/test_diagnostics_mysql.py)；最终结果与限制见 [验收证据](agent-upgrade/diagnostics-verification.json)。

实际验收：新增SQLite专项12通过、MySQL专项4通过；快速Python566通过／19跳过、Node53通过；独立MySQL／Redis全回归1102通过／54跳过。已在当前本机MySQL实跑最近记录、会话、任务三种只读命令，并读取此前隔离桌面证据验证诊断字段。本轮新增模型请求0次，未修改Qt或播放行为，因此未重跑原生桌面层。各层范围重叠，不相加为总体准确率。

面试概括：“我把已有消息、路由、任务、工具和调用账本组织成只读诊断报告，用关联ID及版本定位第一个偏差，区分计划完成、任务预览与设备回执。查询使用一致快照，缺失证据保持未知，不在诊断过程中重试模型或改变状态。”

追问要点：为什么要一致快照；为什么路由成功不等于业务成功；如何避免诊断泄露凭据；为什么查不到任务关联不能直接判失败。
