# 冻结的迁移回归夹具

这些 JSON 用于验证 Python 重写没有改变历史协议，常规测试不需要已退役的 JavaScript 后台、Node 或其 `node_modules`。

| 文件 | 用途 |
| --- | --- |
| `routing_contract_cases.json`、`routing_js_expected.json` | 路由与能力计划的输入和旧版期望 |
| `task_helpers_js_v1.json` | 旧任务辅助逻辑的独立输出 |
| `task_runtime_js_v1.json` | 相同合成输入下，旧工作流的快照、轨迹、调用顺序 |
| `digest_js_v1.json` | 由旧 `storage-task-helpers.mjs` 生成的摘要，覆盖 Unicode、数字序列化与孤立代理字符 |
| `storage_js_v1.json` | 由旧 Store/TaskStore 写出的合成账号、哈希、会话、快照和幂等账本 |

新增两个夹具在删除旧源码前使用原 Node 实现离线生成，`provenance.sourceFiles` 保存对应源码 SHA256。存储夹具生成时，还由原 Node 实际读取过 Python 更新后的密码、会话与快照，三项均通过，结果保留在 `runtimeReadbackAtGeneration`。所有用户名、密码、令牌和记录都只属于随即删除的隔离临时库，没有读取真实配置、用户库或调用外部服务。

现在的存储测试把冻结数据导入运行器自建的 SQLite、PostgreSQL 或 MySQL 测试库，验证 Python 可读，并按冻结的 scrypt、SHA256 和 JSON 协议独立检查当前写入。共享库的独立 `state_version` 列由旧快照中的 `stateVersion` 投影。这是存储格式契约回归，不是通用数据迁移工具，也不再称为实时 Node/Python 双向运行验证。

旧源码路径是来源记录，不是待执行入口。不要为了让测试通过而用当前 Python 输出覆盖这些期望；需要改变协议时新增带版本的夹具并解释兼容策略。上述工程回归不代表模型准确率、真实曲库召回率或播放成功率。
