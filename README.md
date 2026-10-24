# 探歌 · Tango

C++/Qt 桌面音乐播放器与对话音乐助手。当前使用 **Python FastAPI＋Python LangGraph＋MySQL 8.4＋Redis**；网易云查询和账号授权通过设备上的 Node.js 适配服务完成。

项目正在按新路线重做 Agent。统一会话、受控路由及持久音乐任务已接入；真实 MCP、动态 Supervisor 和在线质量闭环仍待实施，尚未完成公网发布验收。

自有代码采用 [MIT 许可证](LICENSE)。第三方组件及音乐内容的许可边界见 [第三方说明](THIRD_PARTY_NOTICES.md)。本仓库发布源码，当前支持与验收以 macOS 开发环境为主。

发布检查、私有数据边界与本次验证见 [开源说明](docs/OPEN_SOURCE_RELEASE.md)。

## 从哪里看

| 需要了解什么 | 当前文档 |
| --- | --- |
| 指定歌曲、歌手作品与场景＋歌手＋时长 | [统一音乐需求](docs/AGENT_MUSIC_REQUIREMENTS.md) · [身份澄清实测](docs/AGENT_IDENTITY_FOLLOWUP_RESULTS.md) |
| Agent 怎么工作、代码在哪里、面试怎么讲 | [当前 Agent 设计](docs/AGENT_DESIGN.md) |
| 每个阶段做了什么、流程、测试与面试问题 | [阶段交付文档索引](docs/STAGE_DELIVERIES.md) |
| 这一版怎样演示、实际验收与简历表述 | [音乐助手演示闭环](docs/AGENT_DEMO_RELEASE.md) |
| 最新测试发现、修复及尚需补什么 | [演示版后续复查](docs/AGENT_DEMO_FOLLOWUP_TESTS.md) |
| 自己查看路由、任务、工具、约束与调用，定位错误 | [只读诊断操作与例子](docs/AGENT_DIAGNOSTICS.md) |
| 配置 LangSmith 云服务，在网页查看一次执行链路 | [LangSmith 接入与查看](docs/LANGSMITH_TRACING.md) |
| 本轮做到哪、实际测试与下一步 | [开发进度](docs/DEVELOPMENT_PROGRESS.md) |
| 用户要求、实施顺序与各步验收 | [升级路线](docs/AGENT_UPGRADE_ROADMAP.md) |
| 五类工程边界及未完成项 | [Harness 规范](docs/HARNESS_ENGINEERING.md) · [能力清单](docs/HARNESS_STATUS.json) |
| 下一阶段真实 MCP | [执行计划](docs/exec-plans/active/mcp-tools.md) |
| Python 配置、接口和启动 | [后台说明](backend_py/README.md) |
| Flash／Plus 按任务选择、密钥与启用开关 | [多模型阶段说明](docs/MODEL_ROUTING.md) |
| 对话开发校准、测试任务与人工评分 | [校准操作与交付](docs/AGENT_CALIBRATION.md) |
| MySQL 事务、迁移与回滚边界 | [存储说明](docs/MYSQL_MIGRATION.md) |
| 开发免登录与正式账号管理 | [开发模式](docs/DEVELOPMENT_MODE.md) · [内测码与额度](docs/BETA_ACCESS_AND_QUOTAS.md) |
| 网易云扫码、接口与授权隔离 | [本机音乐服务](docs/NETEASE_SERVICE.md) |

本次重做的阶段交付文档单独保留，总览不能替代。先前清理时误合并的 5 份阶段说明已经恢复，见 [恢复记录](docs/agent-upgrade/stage-documents-restoration.json)；早期无用资料继续归档。历史 JSON/HTML 的公开摘要保留原版本、失败与成绩，原始运行内容留私有原件，见 [脱敏清单](docs/agent-upgrade/open-source-evidence-manifest.json)；不作为新版效果分数。

## 在 macOS / VS Code 运行

已有本机配置时，双击根目录 `run-macos.command`，或在 VS Code 按 **F5**（需要 CodeLLDB）。也可在命令面板选择“任务: 运行任务”→“运行播放器 · macOS”。**⌘⇧B** 只构建。

```bash
bash scripts/run-macos.sh
```

启动器增量构建 `build-macos/Tango.app`，准备已配置的项目独立 MySQL/Redis，启动 Python 开发后台再打开播放器。默认进入受保护的“本机开发测试”，无需注册，端口为 `16383`；普通后台为 `16382`。开发身份仍受全站与任务预算约束。

首次准备工具和依赖：

```bash
git clone https://github.com/zij30548/Tango.git
cd Tango
brew install qt@5 hiredis mysql@8.4 redis node@24 uv
uv sync --project backend_py --python 3.12 --frozen
bash scripts/setup-music-service.sh
```

全新目录首次启动会创建项目独立的私有 MySQL/Redis 配置与随机凭据；检测到旧账号库时须先按 [存储说明](docs/MYSQL_MIGRATION.md)迁移，启动器不会拿空库替换它。AI 功能需要维护者先在本机按 [后台说明](backend_py/README.md)配置模型密钥。普通后台可用 `bash scripts/start-app-backend.sh` 单独运行。修改模型配置后需重启对应项目后台，不操作其他进程。

维护者只在 `backend_py/.env` 配置模型服务，模板为 [backend_py/.env.example](backend_py/.env.example)。用户无需填写大模型 Key。客户端与 Python 是独立进程，通过 HTTP/JSON 和 SSE 通信；公开部署地址要求 HTTPS。

如果启动日志提示“该端口仍是旧后台”，说明本机后台仍驻留旧版本，启动器会在打开窗口前停止。关闭播放器窗口不会自动结束后台；需要核实项目进程归属后正常重启对应后台，再运行上述入口。2026-10-23 已处理的一次实例见 [启动恢复记录](docs/agent-upgrade/startup-recovery-2026-10-23.json)。

## 当前功能与边界

- **播放与界面**：本地和在线音乐、封面、搜索、历史/收藏双击播放；页面在主窗口切换，底栏显示当前歌曲。透明桌面歌词与主歌词同步；YRC 按真实字词时间高亮，普通 LRC 的行内进度为估算。
- **对话助手**：一个聊天框完成场景找歌、曲库新作查询、偏好提议、解释和连续修改。先预览，明确采用后才更新队列；恢复历史不自动请求模型、写偏好或播放。
- **记忆**：已确认的音乐偏好按探歌账号保存在本机，可编辑和删除。临时场景不自动成为永久偏好；完整历史删除及派生失效仍待完善。
- **专注计划**：保留独立的 1–180 分钟计划，从队列/收藏挑歌，预览后开始。纯音乐等严格条件依赖人工标注；实际播放才扣时，失败曲目有限跳过。成功切换账号会结束计划，普通切歌不推断不喜欢；关闭应用后不自动恢复专注计时或播放。
- **网易云**：登录探歌与连接网易云相互独立。未连接网易云也可检索公开元数据；播放按当前账号实际权益逐曲解析，可能仅试听或不可播。新作查询只覆盖本次曲库返回的有限发行，不能证明全网最新。

固定排行榜是旧演示内容；MV 入口选择本机视频。历史、收藏、偏好和网易云授权仍在设备，没有云同步。默认窗口 1120×780、最小 1000×700；当前沿用 Qt 5，不因后台迁移自动升级 UI 框架。

## 数据与保密

客户端默认数据根目录沿用 `~/Library/Application Support/Zijing/MusicPlayer2/`，避免更名后丢失已有数据：

| 位置 | 用途 |
| --- | --- |
| `history.db` | 本机访客数据 |
| `profiles/<SHA256(profile)>.db` | 各探歌账号的历史、收藏与偏好 |
| `netease/profiles/<账号摘要>/` | 该账号的本机网易云凭据；旧根目录凭据仅属访客 |
| `backend_py/.env`、`backend_py/.data/` | 私有模型配置与后台日志 |
| `.local/mysql-v1/` | 当前项目独立 MySQL/Redis 的私有配置与数据 |
| `.local/storage-v1/` | 旧 PG 源库及备份，不能自动回退或当作当前库 |

测试使用临时目录；可用 `MUSICPLAYER_DATA_DIR` 指定隔离客户端目录。Key、Cookie、会话令牌、数据库、备份、运行日志和编译产物不提交仓库。旧 JS AI 后台已退出当前代码树；`service/` 的 `.mjs` 仍是正在使用的网易云适配器。

## 开发验证

每步遵循 [Harness 规范](docs/HARNESS_ENGINEERING.md)：先定义验收，检查五类边界，测试后记录失败、证据和缺口。

```bash
python3 scripts/check-harness.py
python3 scripts/verify-harness.py --tier quick
# 存储、并发和恢复的独立临时数据库回归
python3 scripts/verify-harness.py --tier mysql
# macOS 原生 Qt 业务流程；模拟上游与生成的静音 WAV
python3 scripts/verify-harness.py --tier native
```

Quick 不覆盖数据库和音频；原生媒体需 macOS 平台，offscreen 通过不能替代音频验收。其他 Qt 用例位于 `tests/*.pro`，按需在独立构建目录运行 qmake/make。内存测量工具为 `scripts/macos_memory_profile.py`；旧客户端样本不能代表当前后台和数据库的总内存。

实际结果以 [开发进度](docs/DEVELOPMENT_PROGRESS.md)和各次冻结报告为准。工程测试不换算 Agent 总体完成率或推荐准确率；新版独立语义效果还需单独评测。真实模型评测须有明确预算。每次云端快速门禁的状态见 [GitHub Actions](https://github.com/zij30548/Tango/actions/workflows/harness.yml)，本机记录不能替代远端结果。
