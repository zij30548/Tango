import { createRequire } from 'node:module';
import path from 'node:path';

const require = createRequire(import.meta.url);

export function createUpstream() {
  const root = path.dirname(require.resolve('NeteaseCloudMusicApi/package.json'));
  const upstreamRequire = createRequire(path.join(root, 'package.json'));
  // 上游允许为各模块传入请求函数；仅导入所需
  // 模块，不导入它的 Express 服务和数百个无关 API。
  const request = upstreamRequire('./util/request.js');
  const { cookieToJson } = upstreamRequire('./util/index.js');
  const axios = upstreamRequire('axios').default;
  axios.defaults.timeout = 15000;
  axios.defaults.maxContentLength = 2 * 1024 * 1024;
  axios.defaults.maxBodyLength = 16384;
  const names = ['login_status', 'login_qr_key', 'login_qr_create', 'login_qr_check',
    'logout', 'cloudsearch', 'song_detail', 'song_url_v1', 'lyric', 'playlist_detail',
    'artist_album', 'artist_songs', 'album', 'lyric_new'];
  return Object.fromEntries(names.map((name) => {
    const module = upstreamRequire(`./module/${name}.js`);
    return [name, (query = {}) => module({
      ...query,
      cookie: cookieToJson(query.cookie || ''),
    }, request)];
  }));
}
