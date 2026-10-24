# 第三方组件与内容说明

探歌 Tango 的自有代码采用根目录 [MIT License](LICENSE)。第三方组件仍归各自权利人所有，适用各自许可证，不因本项目采用 MIT 而改变。本说明按 2026-10-25 本机安装包、许可文件与锁文件核对，列出主要直接依赖；不替代各组件及其传递依赖随包提供的完整 LICENSE／NOTICE。

## 桌面与本机曲库适配

| 组件 | 核对版本 | 许可证与版权 | 官方来源 |
| --- | --- | --- | --- |
| Qt | 5.15.19；项目使用 Qt 5 | 依模块适用 LGPL／GPL 或商业许可；The Qt Company Ltd. 及其他贡献者，保留逐文件声明 | [Qt 许可说明](https://www.qt.io/development/open-source-lgpl-obligations)、[Qt 源码](https://code.qt.io/cgit/) |
| hiredis | 1.5.0 | BSD-3-Clause；Salvatore Sanfilippo、Pieter Noordhuis 及贡献者 | [源码与 COPYING](https://github.com/redis/hiredis/tree/v1.5.0) |
| NeteaseCloudMusicApi | 4.32.0，固定于 `service/package-lock.json` | MIT；Copyright (c) 2013-2022 Binaryify | [npm 固定版本](https://www.npmjs.com/package/NeteaseCloudMusicApi/v/4.32.0)、[上游项目](https://github.com/Binaryify/NeteaseCloudMusicApi) |

Qt 通过 qmake 和系统动态库使用，hiredis 通过系统库链接；仓库不包含这些组件的安装二进制。源码公开不代表已经完成 Qt 安装包分发的许可核验。若分发应用、Qt 库、插件或其他运行时，须按实际包含的模块保留版权、完整许可证及适用的源码／替换库等义务；不能把全部 Qt 模块标为 MIT。

`service/` 是本项目编写的设备侧适配层，上游网易云接口实现来自第三方 NeteaseCloudMusicApi，并非网易云官方开放 SDK。`node_modules/` 不入库，按锁文件安装时应保留依赖包原有许可证。传递依赖还包含 ISC、BSD、0BSD、BlueOak-1.0.0 等许可；例如 node-forge 提供 BSD-3-Clause 或 GPL-2.0 选择。不能把整棵 Node 依赖树视为 MIT。

## Python 后台主要直接依赖

版本与安装完整性以 `backend_py/uv.lock` 为准；下表为本次锁定安装的核对结果。第三方作者和贡献者保留其原始版权，安装包中的 LICENSE／NOTICE 继续适用。

| 依赖 | 核对版本 | 许可证 | 官方来源 |
| --- | --- | --- | --- |
| FastAPI | 0.154.0 | MIT；Sebastián Ramírez | [fastapi/fastapi](https://github.com/fastapi/fastapi) |
| Uvicorn | 0.54.0 | BSD-3-Clause；Encode OSS Ltd | [encode/uvicorn](https://github.com/encode/uvicorn) |
| HTTPX | 0.28.1 | BSD-3-Clause；Encode OSS Ltd | [encode/httpx](https://github.com/encode/httpx) |
| LangGraph | 1.2.16 | MIT；LangChain, Inc. | [langchain-ai/langgraph](https://github.com/langchain-ai/langgraph) |
| LangSmith SDK | 0.14.11 | MIT；原包版权声明 `Copyright 2026 langChain` | [langchain-ai/langsmith-sdk](https://github.com/langchain-ai/langsmith-sdk) |
| asyncpg | 0.32.0 | Apache-2.0；asyncpg authors and contributors | [MagicStack/asyncpg](https://github.com/MagicStack/asyncpg) |
| asyncmy | 0.2.16 | Apache-2.0 | [long2ice/asyncmy](https://github.com/long2ice/asyncmy) |
| cryptography | 47.0.0 | Apache-2.0 OR BSD-3-Clause；Individual contributors | [PyCA 许可说明](https://cryptography.io/en/latest/license/) |
| redis（Python 客户端） | 7.4.1 | MIT；Redis, inc. 及贡献者 | [redis/redis-py](https://github.com/redis/redis-py) |
| python-dotenv | 1.2.4 | BSD-3-Clause；Saurabh Kumar、Ted Tieken、Jacob Kaplan-Moss | [theskumar/python-dotenv](https://github.com/theskumar/python-dotenv) |
| regex | 2026.9.29 | Apache-2.0 AND CNRI-Python | [mrabarnett/mrab-regex](https://github.com/mrabarnett/mrab-regex) |

Python 本身适用 [Python Software Foundation 许可](https://docs.python.org/3/license.html)。若再分发虚拟环境、wheel、OpenSSL 等运行时及传递依赖，应一并保留实际包中的许可文件；本表不是完整二进制发行清单。LangSmith SDK 的许可也不替代 LangSmith 云服务条款。

## 独立数据库服务

MySQL 和 Redis 服务器由用户或部署者另行安装，仓库不携带其服务端源码或二进制。

- **MySQL Community Server 8.4**：适用其 GPL 及随发行版提供的组件许可，版权归 Oracle 及相应权利人；见 [MySQL Community Edition](https://www.mysql.com/products/community/)。通过 asyncmy 连接，不把服务器许可改为本项目 MIT。
- **Redis 服务器**：与上表 MIT 的 `redis` Python 客户端是不同软件。Redis 8 起官方提供 RSALv2／SSPLv1／AGPLv3 许可选择，具体版本与模块以实际发行包为准；见 [Redis 官方许可证](https://redis.io/legal/licenses/)。本项目的 MIT 不覆盖 Redis 服务器，也不替使用者选择其许可。

## 音乐、封面和商标

仓库不附带歌曲音频、商业专辑封面或歌词曲库。音乐、歌词、封面、歌手信息与网易云相关标识仍归相应权利人所有，不属于本项目 MIT 授权。运行时取得的元数据或播放地址不代表获得内容的复制、再分发或商业使用授权；播放以当前账号权益和来源服务规则为准。用户导入的本地文件由用户自行确保有权使用。

## 保留的上游许可正文

以下正文摘自本次安装的原包许可文件；其他组件的完整许可请随实际依赖包保留。

### NeteaseCloudMusicApi 4.32.0 — MIT

```text
The MIT License (MIT)

Copyright (c) 2013-2022 Binaryify

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

### hiredis 1.5.0 — BSD-3-Clause

```text
Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of Redis nor the names of its contributors may be used
  to endorse or promote products derived from this software without specific
  prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```
