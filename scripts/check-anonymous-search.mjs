// 需显式启用的在线诊断：不会加载 backend_py/.env 或已有账号。
// 运行：node scripts/check-anonymous-search.mjs [可选 report.json 路径]
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { once } from 'node:events';

const write = (value) => process.stdout.write(JSON.stringify(value) + '\n');
// 上游库有时会记录原始响应；只保留本脚本允许输出的字段。
for (const method of ['log', 'info', 'warn', 'error', 'debug', 'trace', 'dir'])
  console[method] = () => {};
const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'quiet-anonymous-live-'));
await fs.chmod(directory, 0o700);
process.env.MUSICPLAYER_DATA_DIR = directory;
let server;
try {
  const { openCredentials, privateTemporaryDirectory } = await import('../service/credentials.mjs');
  const credentials = await openCredentials(directory);
  if (credentials.cookie !== '') throw new Error('Anonymous fixture required');
  process.env.TMPDIR = await privateTemporaryDirectory(credentials.directory);
  const { createUpstream } = await import('../service/upstream.mjs');
  const { createMusicServer } = await import('../service/app.mjs');
  server = createMusicServer({ upstream: createUpstream(), credentials, idleTimeoutMs: 0 });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const base = `http://127.0.0.1:${server.address().port}`;
  const request = async (route) => {
    const response = await fetch(base + route, {
      headers: { 'X-MusicPlayer-Token': credentials.token },
      signal: AbortSignal.timeout(20000),
    });
    const data = await response.json();
    if (response.status !== 200)
      return { error: data.error?.code || 'request_failed', status: response.status };
    return data;
  };
  const status = await request('/account/status');
  if (status.loggedIn !== false) throw new Error('Anonymous status check failed');
  const report = {
    testedAt: new Date().toISOString(), loggedIn: false, isolatedCredentials: true,
    applicationApi: '/search -> cloudsearch; /song/url -> song_url_v1', results: [],
  };
  const cases = [
    { artist: '陈奕迅', title: '十年' },
    { artist: '林俊杰', title: '江南' },
    { artist: '孙燕姿', title: '遇见' },
    { artist: '周杰伦', title: '晴天' },
    { artist: 'Taylor Swift', title: 'Love Story' },
    { artist: 'Coldplay', title: 'Yellow' },
    { artist: '毛不易', title: '消愁' },
    { artist: '陈粒', title: '奇妙能力歌' },
  ];
  for (const item of cases) {
    const query = `${item.artist} ${item.title}`;
    const result = { query, searched: false };
    const search = await request('/search?' + new URLSearchParams({ keywords: query, limit: '30', offset: '0' }));
    if (search.error) {
      result.error = search.error;
    } else {
      result.searched = true;
      result.total = search.total;
      result.returned = search.songs.length;
      result.feeCounts = Object.fromEntries([...new Set(search.songs.map(s => s.fee))]
        .map(fee => [String(fee), search.songs.filter(s => s.fee === fee).length]));
      result.topResults = search.songs.slice(0, 3).map(s => ({ id: s.id, title: s.title, artist: s.artist, fee: s.fee }));
      const normalize = value => value.trim().toLocaleLowerCase();
      const target = search.songs.find(song => normalize(song.title) === normalize(item.title) &&
        normalize(song.artist).split(/\s*\/\s*/).includes(normalize(item.artist)));
      result.exactArtistAndTitleInFirstPage = Boolean(target);
      if (target) {
        result.target = { id: target.id, title: target.title, artist: target.artist, fee: target.fee };
        const playback = await request('/song/url?' + new URLSearchParams({ id: String(target.id), level: 'standard' }));
        result.playback = playback.error ? { error: playback.error } : {
          hasUrl: Boolean(playback.url), trial: playback.trial,
          durationMs: playback.durationMs, trialStartMs: playback.trialStartMs,
          trialEndMs: playback.trialEndMs,
        };
      }
    }
    report.results.push(result);
    write(result);
  }
  const page = await request('/search?' + new URLSearchParams({ keywords: '陈奕迅 十年', limit: '30', offset: '30' }));
  report.pagination = page.error ? { error: page.error } : {
    keyword: '陈奕迅 十年', offset: 30, returned: page.songs.length,
    total: page.total, distinctFromFirstPageTop3: page.songs.every(song =>
      !report.results[0].topResults?.some(first => first.id === song.id)),
  };
  report.stillAnonymous = credentials.cookie === '' && (await request('/account/status')).loggedIn === false;
  if (process.argv[2]) await fs.writeFile(process.argv[2], JSON.stringify(report, null, 2) + '\n');
  write({ summary: { searches: report.results.length,
    successful: report.results.filter(r => r.searched).length,
    exactMatches: report.results.filter(r => r.exactArtistAndTitleInFirstPage).length,
    pagination: report.pagination, stillAnonymous: report.stillAnonymous } });
} catch {
  process.stderr.write('匿名实测未能完成；未读取或更改现有登录。\n');
  process.exitCode = 1;
} finally {
  if (server) {
    await new Promise(resolve => { server.close(resolve); server.closeAllConnections(); });
  }
  await fs.rm(directory, { recursive: true, force: true });
}
