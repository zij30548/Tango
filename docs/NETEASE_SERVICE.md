# 网易云账号播放服务

播放器通过本机 Node.js 服务按网易云账号的实际权益获取播放地址。探歌账号与网易云授权相互独立，授权按当前探歌账号的本机空间隔离。界面按接口返回区分完整、试听和不可播；会员身份本身不保证每首歌都能完整播放。

## 准备与运行

需要 Node.js 20 或更新版本（包含 npm）。首次运行，或更新 `service/package-lock.json` 后，在项目目录执行：

```bash
bash scripts/setup-music-service.sh
```

此脚本只在项目 `service/node_modules` 中执行 `npm ci --omit=dev --ignore-scripts`，不安装全局包。使用项目现有启动方式打开播放器，在需要搜索或网易云扫码登录时自动启动本机服务。运行过程中不自动下载依赖；依赖缺失时会提示先运行准备脚本。

服务只监听 `127.0.0.1:16381`，没有使用约 10 分钟后自动退出；下次请求会自动重新启动。可单独执行 `bash scripts/ensure-music-service.sh` 检查启动。服务不输出账号、二维码 key、Cookie 或请求日志。

## 登录与数据位置

使用手机网易云音乐扫描 App 展示的二维码，再在手机确认。密码和短信验证码均不经过本项目。授权凭据由本机服务保管，不返回给 Qt，也不提交到代码仓库。

默认数据目录是：

```text
~/Library/Application Support/Zijing/MusicPlayer2/netease/
  token           本机客户端认证令牌
  cookie.json     旧访客授权（若存在），不自动归属登录的探歌账号
  profiles/<账号摘要>/cookie.json   当前探歌账号的本机网易云授权
  tmp/            上游匿名请求所需的私有临时文件
```

目录权限为 `0700`，令牌和凭据文件为 `0600`。可通过 `MUSICPLAYER_DATA_DIR` 改变 `netease` 的父目录；Qt 与服务必须继承同一设置。客户端通过 `X-MusicPlayer-Profile` 传递账号空间摘要，未传时只访问访客槽。切换探歌账号不会共用上一账号 Cookie；退出网易云只清除当前槽并尝试注销上游。不要分享或提交这个数据目录。

## 接口约定

除 `GET /health` 外，接口均要求 `X-MusicPlayer-Token` 请求头。服务拒绝非本机 Host 和带浏览器 Origin 的请求，不设置 CORS，不暴露通用上游代理。

| 接口 | 请求 | 响应关键字段 |
| --- | --- | --- |
| `GET /health` | 无 | `app: "quiet-netease"`, `version: 1`, `catalogVersion: 2`, `profileIsolation: 1`, `pid` |
| `GET /account/status` | 无 | `loggedIn`, `nickname`, `avatarUrl`, `vipType` |
| `POST /login/qr` | JSON `{}` | `key`, `qrImage`（PNG data URL） |
| `POST /login/qr/check` | JSON `{ "key": "…" }` | `code`, `message` |
| `POST /logout` | JSON `{}` | 与未登录账号状态相同 |
| `GET /search` | `keywords`, `limit=30`, `offset=0` | `songs: [{ id, title, artist, album, coverUrl, durationMs, fee }]`, `total` |
| `GET /radio/search` | `keywords`（最多 200 字）, `limit=5`（1–5）, `offset=0`（0–1000） | `playlists: [{ id, name, coverUrl, trackCount }]`, `total` |
| `GET /radio/playlist` | `id`, `limit=20`（1–20） | `playlist: { id, name, coverUrl, tags, description }`, `songs: [{ id, title, artist, album, coverUrl, durationMs, fee }]` |
| `GET /assistant/artists` | `keywords`（最多 200 字）, `limit=5`（1–5） | `artists: [{ id, name, aliases, coverUrl }]`, `total` |
| `GET /artist/tracks` | `artistId`、`limit=30`（1–30）、`offset=0`（0–1000） | `artistId`, `songs`（含`artists:[{id,name}]`）、`total`（未知为null）、`offset`, `limit`, `more`, `checkedAt` |
| `GET /assistant/releases` | `artistId`, `limit=6`（1–6） | `artist: { id, name }`, `albums: [{ id, name, publishTime, type, coverUrl }]`, `checkedAt` |
| `GET /assistant/album` | `id`, `limit=20`（1–20） | `album: { id, name, publishTime, type, coverUrl, artistId, artistName }`, `songs: [{ id, title, artist, album, coverUrl, durationMs, fee }]` |
| `GET /song/detail` | `id` | `song: { id, title, artist, album, coverUrl, durationMs, fee }` |
| `GET /song/url` | `id`, `level=standard` | `id`, `url`, `trial`, `trialStartMs`, `trialEndMs`, `durationMs`, `level`, `message` |
| `GET /lyrics` | `id` | `lyric`（LRC 文本）、`yrc`（可用时为字词时间） |

扫码状态 `800` 为过期，`801` 为待扫码，`802` 为待手机确认，`803` 为授权成功。返回 `803` 后客户端再获取 `/account/status`，实时验证账户；不在每次扫码轮询中额外请求账号状态。

歌曲 ID 为正整数，最大为 JavaScript 安全整数 `9007199254740991`，Qt 使用 `qint64` 保存。播放位置和时长均为毫秒，试听起止位置由上游的秒转换而来。仅按请求音质获取一次地址，不自动更换歌曲来源、不提升账号权限、不破解试听。`url` 为空表示当前无法播放；`trial: true` 表示上游只提供试听片段。

上游有时返回 `music.126.net` 或其子域名的 HTTP 音频地址；本机服务会将这些已知网易云 CDN 地址的协议升级为 HTTPS，以适配 macOS 原生播放器的安全传输要求。资源路径和查询参数保持原样，其他域名不会自动改写，也不关闭 TLS 验证或放宽 App 的传输安全设置。

POST 请求必须为 JSON，最大 8 KiB；上游请求最多等待 15 秒。错误为相应 HTTP 状态码及 `{ "error": { "code": "…", "message": "…" } }`；不会直接返回上游错误对象。

搜索直接提取 `al.name` / `al.picUrl`，旧收藏与历史记录按稳定歌曲 ID 通过详情补查，不需要更改原数据库。图片 URL 仅接受 `p数字.music.126.net`，HTTP 升为 HTTPS；显示封面不代表获得歌曲完整播放权限。客户端另用没有账号 Cookie 或本机 token 的图片连接获取 512 像素缩略图，最多下载 2 MiB；拒绝重定向和过大原图。本地侧文件与网络图片均在线程池中解码，列表只按可见行加载；内存缓存上限 12 MiB，磁盘缓存 64 MiB，4 并发、64 排队。

电台检索使用 `cloudsearch type=1000` 搜索真实歌单，未登录网易云时也可请求。歌单详情必须与请求 ID 一致，只返回该次详情已有 `tracks` 的前 20 首以内，不按全部 `trackIds` 调用批量详情或 `playlist_track_all`，不承诺取得完整歌单；空歌单返回空数组，缺失曲目数组或错误 ID 报上游响应无效。歌单名最多 160 字，标签最多 6 个、每个 40 字，描述最多 500 字，封面沿用同一 CDN 校验。这些元数据和 `fee` 不代表播放权益，实际播放仍需逐首调用 `/song/url`。歌单标签、描述只是检索证据，不保证单首歌曲的节奏、情绪或纯音乐属性。

助手的新作查询使用 `cloudsearch type=100` 查歌手，再以选定的实际歌手 ID 调用 `artist_album` 获取一页最多 6 张发行记录，按 `publishTime` 降序排列。不会扫描完整目录或搜索全网，因此只能称为“本次网易云曲库返回的最近发行”，不能据此断言全网最新或最近没有发歌。同名歌手可能对应不同 ID，应按姓名、别名核对身份；不能仅凭搜索结果包含同名账号就合并其作品。服务要求发行列表返回的歌手 ID 与请求一致，丢弃重复专辑和无效、未来日期。`publishTime` 是上游专辑发行的 Unix 毫秒时间戳，`checkedAt` 是本次请求完成时服务端核验时间，二者不能混用。

`/assistant/album` 只读取请求专辑本次详情中前 20 条以内的歌曲，不扩展整张专辑目录。详情的专辑 ID、发行日期和歌手身份必须有效，未来发行直接报错，空曲目数组允许返回。元数据沿用图片域名和正安全整数 ID 校验，歌曲没有单独的专辑名或封面时才使用已校验的专辑信息补全。不返回音频地址，不推断播放权限。三个助手接口均允许未登录网易云账号时检索公开元数据，并保留本机令牌、Host、Origin、超时和错误脱敏约束。

## 依赖来源与验证

使用原包 [`NeteaseCloudMusicApi`](https://www.npmjs.com/package/NeteaseCloudMusicApi)，固定版本 `4.32.0`，发布者元数据为 `traveller <binaryify@gmail.com>`，包内作者 `binaryify`。精确版本、所有传递依赖及下载完整性摘要保存在 `service/package-lock.json`。没有使用 Enhanced、Unblock 或第三方歌曲匹配源；只加载扫码、账号、搜索、歌单详情、歌手发行、专辑详情、歌曲详情、授权地址和歌词的 14 个上游模块，不启动上游的通用 Express API 服务。接口依据[上游项目文档](https://binaryify.github.io/NeteaseCloudMusicApi/)及当前依赖源码，这是第三方适配项目，并非网易云官方开放 SDK。

```bash
cd service
npm test
```

测试使用模拟上游，无需真实账号或网络，覆盖本机访问限制、令牌和账号空间、大小/数量/ID、授权凭据写入、注销竞态、试听、歌词和脱敏。统一快速门禁还会运行服务与开发环境测试：`python3 scripts/verify-harness.py --tier quick`。实际执行证据见 [开发进度](DEVELOPMENT_PROGRESS.md)。

`GET /lyrics` 优先读取 YRC，非超时失败最多回退一次旧歌词接口。YRC 有真实字词时间时渐进高亮；只有 LRC 时行内进度为估算，不伪称逐字时间准确。

旧版匿名查询、扫码和单曲会员播放记录只代表当时结果，原文已归档，映射见 [文档整理记录](agent-upgrade/document-cleanup-verification.json)。本轮文档整理没有重测真实曲库或会员播放；正式发布仍需单独核验部署、曲库接入条件和账号权益。


2026-10-24：元数据保留上游实际 `artists:[{id,name}]`，展示姓名与授权身份分开。`/artist/tracks` 使用限定的 `artist_songs`，要求返回身份、分页和署名一致；缺少总数时只有明确 `more` 才可继续，不猜测完整目录。普通作品浏览默认12、最多30首；场景任务先搜场景来源，不足时最多补两页／40候选。明确别名来自上游alias/trans字段，不用姓名子串放宽身份。新版Qt要求 `catalogVersion:2`，旧适配服务需正常重启；不自动终止未知端口进程。见[本阶段](AGENT_MUSIC_REQUIREMENTS.md)。
