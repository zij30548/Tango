import http from 'node:http';
import { timingSafeEqual } from 'node:crypto';

const APP = 'quiet-netease';
const MAX_BODY = 8192;
const LEVELS = new Set(['standard', 'exhigh', 'lossless', 'hires', 'jyeffect', 'sky', 'jymaster']);
const LOGGED_OUT = Object.freeze({ loggedIn: false, nickname: '', avatarUrl: '', vipType: 0 });

class ApiError extends Error {
  constructor(status, code, message) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

function invalid(message = '请求参数无效') {
  return new ApiError(400, 'invalid_request', message);
}

function safeNumber(value, minimum = 0, maximum = Number.MAX_SAFE_INTEGER) {
  const text = String(value ?? '');
  if (!/^\d+$/.test(text)) throw invalid();
  const number = Number(text);
  if (!Number.isSafeInteger(number) || number < minimum || number > maximum) throw invalid();
  return number;
}

function milliseconds(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? Math.round(number) : 0;
}

function coverUrlFrom(value) {
  if (typeof value !== 'string' || value.length > 2048 ||
      !/^https?:\/\//i.test(value) || /[\s\\]/.test(value)) return '';
  try {
    const url = new URL(value);
    // 只支持网易云专辑封面使用的图片 CDN。
    // 不要让元数据变成桌面端可任意请求的图片地址。
    if (!['http:', 'https:'].includes(url.protocol) ||
        !/^p\d+\.music\.126\.net$/i.test(url.hostname) ||
        url.username || url.password || url.port || url.hash) return '';
    url.protocol = 'https:';
    return url.href;
  } catch {
    return '';
  }
}

function metadataFrom(song) {
  if (!song || !Number.isSafeInteger(song.id) || song.id <= 0) return null;
  const artists = Array.isArray(song.ar) ? song.ar : Array.isArray(song.artists) ? song.artists : [];
  // 展示名不能充当身份。保留上游真实署名 ID，缺失时返回空数组而不是从检索词伪造。
  const seenArtists = new Set();
  const credits = artists.slice(0, 12).filter((artist) => artist &&
    Number.isSafeInteger(artist.id) && artist.id > 0 && typeof artist.name === 'string' &&
    artist.name.trim() && !seenArtists.has(artist.id) && seenArtists.add(artist.id))
    .map((artist) => ({ id: artist.id, name: artist.name.trim().slice(0, 120) }));
  return {
    id: song.id,
    title: String(song.name || '').slice(0, 500),
    artist: artists.slice(0, 30).map((artist) => String(artist?.name || '').slice(0, 200)).join(' / '),
    artists: credits,
    album: String(song.al?.name || song.album?.name || '').slice(0, 500),
    coverUrl: coverUrlFrom(song.al?.picUrl) || coverUrlFrom(song.album?.picUrl),
    durationMs: milliseconds(song.dt ?? song.duration),
    fee: Number.isInteger(song.fee) ? song.fee : 0,
  };
}

function playlistMetadataFrom(playlist) {
  if (!playlist || !Number.isSafeInteger(playlist.id) || playlist.id <= 0) return null;
  return {
    id: playlist.id,
    name: typeof playlist.name === 'string' ? playlist.name.slice(0, 160) : '',
    coverUrl: coverUrlFrom(playlist.coverImgUrl),
    trackCount: Number.isSafeInteger(playlist.trackCount) && playlist.trackCount >= 0
      ? playlist.trackCount : 0,
  };
}

function artistMetadataFrom(artist) {
  if (!artist || !Number.isSafeInteger(artist.id) || artist.id <= 0 ||
      typeof artist.name !== 'string' || !artist.name.trim()) return null;
  return {
    id: artist.id,
    name: artist.name.trim().slice(0, 160),
    // 网易云不同接口分别使用 alias / trans / transNames；仅合并上游明确给出的名称。
    aliases: [...new Set(['alias', 'trans', 'transNames'].flatMap((key) =>
      (Array.isArray(artist[key]) ? artist[key] : []).slice(0, 6))
      .filter((alias) => typeof alias === 'string')
      .map((alias) => alias.trim().slice(0, 100)).filter(Boolean))].slice(0, 10),
    coverUrl: coverUrlFrom(artist.picUrl) || coverUrlFrom(artist.img1v1Url),
  };
}

function albumMetadataFrom(album, checkedAt) {
  if (!album || !Number.isSafeInteger(album.id) || album.id <= 0 ||
      typeof album.name !== 'string' || !album.name.trim() ||
      !Number.isSafeInteger(album.publishTime) || album.publishTime <= 0 ||
      album.publishTime > checkedAt) return null;
  return {
    id: album.id,
    name: album.name.trim().slice(0, 160),
    publishTime: album.publishTime,
    type: typeof album.type === 'string' ? album.type.slice(0, 40) : '',
    coverUrl: coverUrlFrom(album.picUrl),
  };
}

function accountFrom(body) {
  const data = body?.data || body;
  const profile = data?.profile;
  if (!profile || !data?.account || data.code && data.code !== 200) return { ...LOGGED_OUT };
  return {
    loggedIn: true,
    nickname: String(profile.nickname || '').slice(0, 200),
    avatarUrl: /^https?:\/\//.test(profile.avatarUrl || '') ? String(profile.avatarUrl) : '',
    vipType: Number.isSafeInteger(profile.vipType) ? profile.vipType : 0,
  };
}

export function normalizeSongUrl(body, id, requestedLevel) {
  const song = body?.data?.[0];
  if (!song || Number(song.id) !== id) {
    throw new ApiError(502, 'invalid_upstream_response', '网易云没有返回有效的播放信息');
  }
  let url = typeof song.url === 'string' && /^https?:\/\//i.test(song.url) ? song.url : '';
  if (url) {
    try {
      const parsed = new URL(url);
      if (parsed.protocol === 'http:' && (parsed.hostname === 'music.126.net' ||
          parsed.hostname.endsWith('.music.126.net'))) {
        // 网易云音频 CDN 支持 HTTPS；只修改协议，以保证签名
        // 路径、转义字符及查询参数逐字节保持不变。
        url = url.replace(/^http:/i, 'https:');
      }
    } catch {
      url = '';
    }
  }
  const trial = Boolean(song.freeTrialInfo);
  return {
    id, url, trial,
    // 上游 freeTrialInfo 的位置单位是秒，Qt 和 LRC 使用毫秒。
    trialStartMs: milliseconds(Number(song.freeTrialInfo?.start || 0) * 1000),
    trialEndMs: milliseconds(Number(song.freeTrialInfo?.end || 0) * 1000),
    durationMs: milliseconds(song.time),
    level: typeof song.level === 'string' ? song.level : requestedLevel,
    message: !url ? '当前账号暂无这首歌的播放权限，可在网易云音乐中查看' :
      trial ? '网易云仅为当前账号提供试听片段' : '',
  };
}

function authorized(header, expected) {
  if (typeof header !== 'string') return false;
  const candidate = Buffer.from(header);
  const token = Buffer.from(expected);
  return candidate.length === token.length && timingSafeEqual(candidate, token);
}

function respond(response, status, value) {
  if (response.destroyed || response.writableEnded) return;
  const body = JSON.stringify(value);
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    // 不返回 CORS 响应头；只支持原生客户端调用。
  });
  response.end(body);
}

async function readBody(request) {
  if (!/^application\/json(?:\s*;.*)?$/i.test(request.headers['content-type'] || '')) {
    throw new ApiError(415, 'content_type', '请求必须使用 JSON');
  }
  if (Number(request.headers['content-length'] || 0) > MAX_BODY) {
    throw new ApiError(413, 'body_too_large', '请求过大');
  }
  let length = 0;
  const parts = [];
  for await (const part of request) {
    length += part.length;
    if (length > MAX_BODY) throw new ApiError(413, 'body_too_large', '请求过大');
    parts.push(part);
  }
  try {
    const body = JSON.parse(Buffer.concat(parts).toString('utf8') || '{}');
    if (!body || Array.isArray(body) || typeof body !== 'object') throw invalid();
    return body;
  } catch {
    throw invalid('JSON 格式无效');
  }
}

function cookieFrom(result) {
  // 只保留 Set-Cookie 的 name=value 键值对，不保留 Path 等属性。
  const values = Array.isArray(result.cookie) ? result.cookie : [];
  const pairs = values.map((value) => String(value).split(';', 1)[0].trim())
    .filter((value) => /^[A-Za-z0-9_]+=[^;\r\n]*$/.test(value));
  if (!pairs.some((pair) => pair.startsWith('MUSIC_U=') && pair.length > 8)) {
    throw new ApiError(502, 'missing_login_cookie', '网易云未返回有效的登录凭据，请重新扫码');
  }
  return pairs.join('; ');
}

export function createMusicServer({ upstream, credentials, timeoutMs = 15000, idleTimeoutMs = 600000 }) {
  // 二维码版本限定在与请求相同、不可变的账号配置范围内。
  // 账号 A 的退出操作或待确认二维码，不能授权或取消账号 B 的操作。
  const sessions = new Map();
  let activeUpstream = 0;
  let idleTimer;

  function touch() {
    clearTimeout(idleTimer);
    if (!idleTimeoutMs) return;
    idleTimer = setTimeout(() => {
      if (activeUpstream) return touch();
      server.close();
      server.closeAllConnections();
    }, idleTimeoutMs);
    idleTimer.unref();
  }

  async function call(name, query) {
    let timer;
    ++activeUpstream;
    try {
      return await Promise.race([
        Promise.resolve().then(() => upstream[name](query)),
        new Promise((_, reject) => {
          timer = setTimeout(() => reject(new ApiError(504, 'upstream_timeout', '网易云请求超时，请重试')), timeoutMs);
          timer.unref();
        }),
      ]);
    } catch (error) {
      if (error instanceof ApiError) throw error;
      // 上游错误可能包含请求头、二维码密钥和 Cookie；不得
      // 返回或记录其原始消息、响应体、堆栈或 Axios 配置。
      throw new ApiError(502, 'upstream_unavailable', '网易云服务暂时不可用，请稍后重试');
    } finally {
      clearTimeout(timer);
      --activeUpstream;
      touch();
    }
  }

  const server = http.createServer(async (request, response) => {
    try {
      const port = server.address().port;
      if (request.headers.host !== `127.0.0.1:${port}` || request.headers.origin) {
        throw new ApiError(403, 'local_client_only', '仅支持本机播放器访问');
      }
      const url = new URL(request.url, `http://127.0.0.1:${port}`);
      if (request.method === 'GET' && url.pathname === '/health' && !url.search) {
        touch();
        return respond(response, 200, { app: APP, version: 1, profileIsolation: 1, catalogVersion: 2, pid: process.pid });
      }
      if (!authorized(request.headers['x-musicplayer-token'], credentials.token)) {
        throw new ApiError(401, 'unauthorized', '本机服务验证失败，请重新启动播放器');
      }
      touch();
      const route = `${request.method} ${url.pathname}`;
      const profile = request.headers['x-musicplayer-profile'] ?? 'guest';
      if (typeof profile !== 'string' || !(profile === 'guest' || /^[a-f0-9]{64}$/.test(profile))) {
        throw invalid('本机音乐账号范围无效');
      }
      if (!sessions.has(profile) && sessions.size >= 128) {
        throw new ApiError(429, 'profile_limit', '本机账号数量过多，请重新启动音乐服务');
      }
      const credentialSlot = typeof credentials.forProfile === 'function'
        ? await credentials.forProfile(profile)
        : profile === 'guest' ? credentials : null;
      if (!credentialSlot) throw invalid('本机音乐服务尚未支持账号隔离');
      if (!sessions.has(profile)) sessions.set(profile, { revision: 0, pendingQr: null });
      const session = sessions.get(profile);
      const cookie = credentialSlot.cookie;
      if (route === 'GET /account/status') {
        if (!cookie) return respond(response, 200, { ...LOGGED_OUT });
        const revision = session.revision;
        const result = await call('login_status', { cookie });
        if (revision !== session.revision || cookie !== credentialSlot.cookie) {
          return respond(response, 200, { ...LOGGED_OUT });
        }
        // 每次显式查询状态都进行在线验证；不信任过期的
        // 磁盘配置，也不因持有 Cookie 就认定已登录。
        return respond(response, 200, accountFrom(result.body));
      }
      if (route === 'POST /login/qr') {
        await readBody(request);
        const revision = ++session.revision;
        session.pendingQr = null;
        const result = await call('login_qr_key', {});
        const key = result.body?.data?.unikey;
        if (typeof key !== 'string' || !/^[A-Za-z0-9_-]{1,256}$/.test(key)) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云未返回有效的登录二维码');
        }
        const imageResult = await call('login_qr_create', { key, qrimg: true, platform: 'pc' });
        const qrImage = imageResult.body?.data?.qrimg;
        if (typeof qrImage !== 'string' || !qrImage.startsWith('data:image/png;base64,')) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云未返回有效的登录二维码');
        }
        if (revision !== session.revision) throw invalid('本次登录已取消');
        session.pendingQr = { key, revision, expiresAt: Date.now() + 180000 };
        return respond(response, 200, { key, qrImage });
      }
      if (route === 'POST /login/qr/check') {
        const { key } = await readBody(request);
        if (!session.pendingQr || key !== session.pendingQr.key || Date.now() >= session.pendingQr.expiresAt) {
          return respond(response, 200, { code: 800, message: '二维码已过期，请刷新' });
        }
        const revision = session.pendingQr.revision;
        const result = await call('login_qr_check', { key });
        if (revision !== session.revision) return respond(response, 200, { code: 800, message: '本次登录已取消' });
        const code = Number(result.body?.code);
        const messages = { 800: '二维码已过期，请刷新', 801: '等待网易云音乐扫码',
          802: '请在手机上确认登录', 803: '已授权登录' };
        if (!Object.hasOwn(messages, code)) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云未返回有效的扫码状态，请重试');
        }
        if (code === 803) {
          await credentialSlot.saveCookie(cookieFrom(result));
          if (revision !== session.revision) return respond(response, 200, { code: 800, message: '本次登录已取消' });
          ++session.revision;
          session.pendingQr = null;
        } else if (code === 800) {
          session.pendingQr = null;
        }
        return respond(response, 200, { code, message: messages[code] });
      }
      if (route === 'POST /logout') {
        await readBody(request);
        ++session.revision;
        session.pendingQr = null;
        await credentialSlot.clearCookie();
        if (cookie) {
          try { await call('logout', { cookie }); } catch { /* 始终以本机退出登录为准。 */ }
        }
        return respond(response, 200, { ...LOGGED_OUT });
      }
      if (route === 'GET /search') {
        const keywords = (url.searchParams.get('keywords') || '').trim();
        if (!keywords || keywords.length > 200) throw invalid('请输入不超过 200 字的歌名或歌手');
        const limit = safeNumber(url.searchParams.get('limit') || 30, 1, 100);
        const offset = safeNumber(url.searchParams.get('offset') || 0, 0, 1000000);
        const result = await call('cloudsearch', { keywords, limit, offset, type: 1, cookie });
        if (result.body?.code !== 200 || !result.body?.result) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云搜索暂时不可用，请重试');
        }
        const raw = result.body.result;
        const songs = (Array.isArray(raw.songs) ? raw.songs : []).slice(0, limit)
          .map(metadataFrom).filter(Boolean);
        return respond(response, 200, { songs, total: milliseconds(raw.songCount) });
      }
      if (route === 'GET /radio/search') {
        const keywords = (url.searchParams.get('keywords') || '').trim();
        if (!keywords || keywords.length > 200) throw invalid('请输入不超过 200 字的歌单关键词');
        const limit = safeNumber(url.searchParams.get('limit') ?? 5, 1, 5);
        const offset = safeNumber(url.searchParams.get('offset') ?? 0, 0, 1000);
        const result = await call('cloudsearch', { keywords, limit, offset, type: 1000, cookie });
        const raw = result.body?.result;
        if (result.body?.code !== 200 || !raw ||
            (!Array.isArray(raw.playlists) && !(raw.playlists === undefined && raw.playlistCount === 0))) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云歌单搜索暂时不可用，请重试');
        }
        const playlists = (raw.playlists || []).slice(0, limit)
          .map(playlistMetadataFrom).filter(Boolean);
        const total = Number.isSafeInteger(raw.playlistCount) && raw.playlistCount >= 0
          ? raw.playlistCount : 0;
        return respond(response, 200, { playlists, total });
      }
      if (route === 'GET /radio/playlist') {
        const id = safeNumber(url.searchParams.get('id'), 1);
        const limit = safeNumber(url.searchParams.get('limit') ?? 20, 1, 20);
        const artist = (url.searchParams.get('artist') || '').trim();
        if (artist.length > 100 || url.searchParams.has('artist') && !artist) throw invalid('歌手筛选参数无效');
        const artistId = url.searchParams.has('artistId') ? safeNumber(url.searchParams.get('artistId'), 1) : 0;
        if (artistId && artist) throw invalid('歌手筛选只能使用一种身份条件');
        const result = await call('playlist_detail', { id, cookie });
        const raw = result.body?.playlist;
        const metadata = playlistMetadataFrom(raw);
        if (result.body?.code !== 200 || !metadata || metadata.id !== id || !Array.isArray(raw.tracks)) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云未返回有效的歌单信息，请重试');
        }
        // 先筛选已经返回的公开元数据，再限制响应数量。
        // 混合歌单中，某位歌手可能在第20首之后才出现；不会
        // 进一步展开歌曲 ID 或获取音频，扫描数量仍有上限。
        const canonical = (value) => value.normalize('NFKC').replace(/[\s.·]/g, '').toLowerCase();
        const wanted = canonical(artist);
        const songs = raw.tracks.slice(0, artist || artistId ? 2000 : limit).map(metadataFrom).filter(Boolean)
          .filter((song) => artistId ? song.artists.some((credit) => credit.id === artistId) :
            !artist || song.artist.split(/\s*(?:\/|、|,|，|&|;|；| feat\.? | featuring )\s*/i)
              .some((credit) => canonical(credit) === wanted)).slice(0, limit);
        const tags = [...new Set((Array.isArray(raw.tags) ? raw.tags : [])
          .slice(0, 6).filter((tag) => typeof tag === 'string')
          .map((tag) => tag.trim().slice(0, 40)).filter(Boolean))];
        return respond(response, 200, { playlist: {
          id: metadata.id, name: metadata.name, coverUrl: metadata.coverUrl, tags,
          description: typeof raw.description === 'string' ? raw.description.slice(0, 500) : '',
        }, songs });
      }
      if (route === 'GET /assistant/artists') {
        const keywords = (url.searchParams.get('keywords') || '').trim();
        if (!keywords || keywords.length > 200) throw invalid('请输入不超过 200 字的歌手名');
        const limit = safeNumber(url.searchParams.get('limit') ?? 5, 1, 5);
        const result = await call('cloudsearch', { keywords, limit, offset: 0, type: 100, cookie });
        const raw = result.body?.result;
        if (result.body?.code !== 200 || !raw ||
            (!Array.isArray(raw.artists) && !(raw.artists === undefined && raw.artistCount === 0))) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云歌手搜索暂时不可用，请重试');
        }
        const seen = new Set();
        const artists = (raw.artists || []).slice(0, limit).map(artistMetadataFrom)
          .filter((artist) => artist && !seen.has(artist.id) && seen.add(artist.id));
        const total = Number.isSafeInteger(raw.artistCount) && raw.artistCount >= 0
          ? raw.artistCount : 0;
        return respond(response, 200, { artists, total });
      }
      if (route === 'GET /artist/tracks') {
        const artistId = safeNumber(url.searchParams.get('artistId'), 1);
        const limit = safeNumber(url.searchParams.get('limit') ?? 30, 1, 30);
        const offset = safeNumber(url.searchParams.get('offset') ?? 0, 0, 1000);
        const result = await call('artist_songs', { id: artistId, limit, offset, order: 'hot', cookie });
        const raw = result.body;
        if (raw?.code !== 200 || !Array.isArray(raw.songs) ||
            raw.total !== undefined && (!Number.isSafeInteger(raw.total) || raw.total < 0) ||
            raw.more !== undefined && typeof raw.more !== 'boolean' ||
            raw.total === undefined && raw.more === undefined) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云未返回有效的歌手作品，请重试');
        }
        const seen = new Set();
        // 只按上游署名 ID 认领作品；同名、标题提及和缺失署名都不能冒充目标歌手。
        const songs = raw.songs.slice(0, limit).map(metadataFrom).filter((song) => song &&
          song.artists.some((credit) => credit.id === artistId) && !seen.has(song.id) && seen.add(song.id));
        // 缺少总数时明确返回未知；不能把当前页数量冒充整个歌手的作品总数。
        const total = raw.total ?? null;
        const more = raw.more ?? offset + raw.songs.slice(0, limit).length < total;
        return respond(response, 200, { artistId, songs, total, offset, limit, more, checkedAt: Date.now() });
      }
      if (route === 'GET /assistant/releases') {
        const artistId = safeNumber(url.searchParams.get('artistId'), 1);
        const limit = safeNumber(url.searchParams.get('limit') ?? 6, 1, 6);
        // 只读取当前曲库中有数量上限的一页，不扫描整个曲库。
        const result = await call('artist_album', { id: artistId, limit, offset: 0, cookie });
        const raw = result.body;
        const artist = artistMetadataFrom(raw?.artist);
        if (raw?.code !== 200 || !artist || artist.id !== artistId || !Array.isArray(raw.hotAlbums)) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云未返回有效的歌手发行信息，请重试');
        }
        const checkedAt = Date.now();
        const seen = new Set();
        const albums = raw.hotAlbums.slice(0, limit)
          .map((album) => albumMetadataFrom(album, checkedAt))
          .filter((album) => album && !seen.has(album.id) && seen.add(album.id))
          .sort((left, right) => right.publishTime - left.publishTime || right.id - left.id);
        return respond(response, 200, { artist: { id: artist.id, name: artist.name }, albums, checkedAt });
      }
      if (route === 'GET /assistant/album') {
        const id = safeNumber(url.searchParams.get('id'), 1);
        const limit = safeNumber(url.searchParams.get('limit') ?? 20, 1, 20);
        const result = await call('album', { id, cookie });
        const raw = result.body;
        const metadata = albumMetadataFrom(raw?.album, Date.now());
        const artist = artistMetadataFrom(raw?.album?.artist) ||
          artistMetadataFrom(Array.isArray(raw?.album?.artists) ? raw.album.artists[0] : null);
        if (raw?.code !== 200 || !metadata || metadata.id !== id || !artist || !Array.isArray(raw.songs)) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云未返回有效的专辑信息，请重试');
        }
        const seen = new Set();
        const songs = raw.songs.slice(0, limit).map(metadataFrom)
          .filter((song) => song && !seen.has(song.id) && seen.add(song.id))
          .map((song) => ({ ...song,
            album: song.album || metadata.name,
            coverUrl: song.coverUrl || metadata.coverUrl,
          }));
        return respond(response, 200, { album: { ...metadata,
          artistId: artist.id, artistName: artist.name,
        }, songs });
      }
      if (route === 'GET /song/detail') {
        const id = safeNumber(url.searchParams.get('id'), 1);
        const result = await call('song_detail', { ids: String(id), cookie });
        if (result.body?.code !== 200 || !Array.isArray(result.body?.songs)) {
          throw new ApiError(502, 'invalid_upstream_response', '网易云歌曲信息暂时不可用，请重试');
        }
        const song = metadataFrom(result.body.songs.find((entry) => entry?.id === id));
        if (!song) throw new ApiError(404, 'song_not_found', '网易云暂无这首歌的信息');
        return respond(response, 200, { song });
      }
      if (route === 'GET /song/url') {
        const id = safeNumber(url.searchParams.get('id'), 1);
        const level = url.searchParams.get('level') || 'standard';
        if (!LEVELS.has(level)) throw invalid('不支持的音质');
        const result = await call('song_url_v1', { id, level, cookie });
        return respond(response, 200, normalizeSongUrl(result.body, id, level));
      }
      if (route === 'GET /lyrics') {
        const id = safeNumber(url.searchParams.get('id'), 1);
        let result;
        // 新版歌词包含真实的逐字时间戳（YRC）；仍兼容
        // 旧曲库或旧服务，但不因上游超时自动重试。
        if (typeof upstream.lyric_new === 'function') {
          try {
            result = await call('lyric_new', { id, cookie });
          } catch (error) {
            if (error instanceof ApiError && error.status === 504) throw error;
          }
        }
        if (result?.body?.code !== 200) result = await call('lyric', { id, cookie });
        if (result.body?.code !== 200) throw new ApiError(502, 'invalid_upstream_response', '网易云歌词暂时不可用');
        const text = (value) => typeof value === 'string' ? value.slice(0, 512 * 1024) : '';
        return respond(response, 200, {
          lyric: text(result.body?.lrc?.lyric), yrc: text(result.body?.yrc?.lyric),
        });
      }
      throw new ApiError(404, 'not_found', '接口不存在');
    } catch (error) {
      const known = error instanceof ApiError;
      respond(response, known ? error.status : 500, {
        error: {
          code: known ? error.code : 'service_error',
          message: known ? error.message : '本机音乐服务暂时不可用，请重新启动播放器',
        },
      });
    }
  });
  server.requestTimeout = 15000;
  server.headersTimeout = 10000;
  server.keepAliveTimeout = 1000;
  server.maxHeadersCount = 32;
  server.on('listening', touch);
  server.on('close', () => clearTimeout(idleTimer));
  return server;
}
