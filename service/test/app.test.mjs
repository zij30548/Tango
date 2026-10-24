import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { once } from 'node:events';
import { createMusicServer, normalizeSongUrl } from '../app.mjs';
import { openCredentials } from '../credentials.mjs';

const TRACK_ID = 5123456789;
const PLAYLIST_ID = 6123456789;

async function fixture(t, overrides = {}, options = {}) {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'quiet-netease-test-'));
  const credentials = await openCredentials(directory);
  const calls = [];
  const defaults = {
    login_status: { body: { data: { code: 200, account: { id: 123 }, profile: { nickname: '测试账号', vipType: 11 } } } },
    login_qr_key: { body: { data: { unikey: 'private_qr_key' } } },
    login_qr_create: { body: { data: { qrimg: 'data:image/png;base64,dGVzdA==' } } },
    login_qr_check: { body: { code: 803 }, cookie: ['MUSIC_U=private_account_cookie; Path=/; HttpOnly', '__csrf=csrf_value; Secure'] },
    logout: { body: { code: 200 } },
    cloudsearch: { body: { code: 200, result: { songCount: 1, songs: [{ id: TRACK_ID, name: '测试歌曲', ar: [{ name: '测试歌手' }], al: { name: '测试专辑', picUrl: 'http://p3.music.126.net/album.jpg' }, dt: 210000, fee: 1 }] } } },
    song_detail: { body: { code: 200, songs: [{ id: TRACK_ID, name: '测试歌曲', ar: [{ name: '测试歌手' }], al: { name: '测试专辑', picUrl: 'http://p3.music.126.net/album.jpg' }, dt: 210000, fee: 1 }], cookie: 'provider_secret' } },
    playlist_detail: { body: { code: 200, playlist: {
      id: PLAYLIST_ID, name: '雨天轻音乐', coverImgUrl: 'http://p4.music.126.net/playlist.jpg',
      tags: ['轻音乐', '学习'], description: '适合雨天的歌单。', trackCount: 1,
      tracks: [{ id: TRACK_ID, name: '测试歌曲', ar: [{ name: '测试歌手' }],
        al: { name: '测试专辑', picUrl: 'http://p3.music.126.net/album.jpg' }, dt: 210000, fee: 1 }],
    } } },
    song_url_v1: { body: { code: 200, data: [{ id: TRACK_ID, url: 'https://m10.music.126.net/test.mp3', level: 'standard', time: 210000, freeTrialInfo: { start: 30, end: 60 } }] } },
    lyric: { body: { code: 200, lrc: { lyric: '[00:01.00]测试歌词' } } },
  };
  const upstream = Object.fromEntries(Object.entries({ ...defaults, ...overrides }).map(([name, result]) =>
    [name, async (query) => {
      calls.push({ name, query });
      return typeof result === 'function' ? result(query) : structuredClone(result);
    }]));
  const server = createMusicServer({ upstream, credentials, idleTimeoutMs: 0, ...options });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const base = `http://127.0.0.1:${server.address().port}`;
  t.after(async () => {
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
    await fs.rm(directory, { recursive: true, force: true });
  });
  async function request(route, { authenticated = true, method = 'GET', body, headers = {} } = {}) {
    const response = await fetch(base + route, {
      method,
      headers: {
        ...(authenticated ? { 'X-MusicPlayer-Token': credentials.token } : {}),
        ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}),
        ...headers,
      },
      ...(body !== undefined ? { body: typeof body === 'string' ? body : JSON.stringify(body) } : {}),
    });
    return { status: response.status, headers: response.headers, data: await response.json() };
  }
  return { credentials, calls, request, directory, server };
}

test('health is public but API is token protected, loopback only and has no CORS', async (t) => {
  const { request, calls, server } = await fixture(t);
  assert.equal(server.address().address, '127.0.0.1');
  const health = await request('/health', { authenticated: false });
  assert.equal(health.status, 200);
  assert.equal(health.data.app, 'quiet-netease');
  assert.equal(health.data.version, 1);
  assert.equal(health.data.profileIsolation, 1);
  assert.equal(health.data.catalogVersion, 2);
  assert.equal(health.headers.get('access-control-allow-origin'), null);
  assert.equal((await request('/account/status', { authenticated: false })).status, 401);
  assert.equal((await request('/account/status', { headers: { 'X-MusicPlayer-Token': 'wrong' } })).status, 401);
  assert.equal((await request('/health', { headers: { Origin: 'https://example.com' } })).status, 403);
  const invalidHost = await new Promise((resolve, reject) => {
    http.get({ hostname: '127.0.0.1', port: server.address().port, path: '/health',
      headers: { Host: 'attacker.example' } }, (response) => {
      response.resume();
      resolve(response.statusCode);
    }).on('error', reject);
  });
  assert.equal(invalidHost, 403);
  assert.equal((await request('/api?uri=/arbitrary')).status, 404);
  assert.equal((await request('/song/url?id=1', { method: 'POST', body: {} })).status, 404);
  assert.equal(calls.length, 0);
});

test('JSON body limits and safe integer IDs are enforced before upstream', async (t) => {
  const { request, calls } = await fixture(t);
  assert.equal((await request('/login/qr', { method: 'POST', body: '{broken' })).status, 400);
  assert.equal((await request('/login/qr', { method: 'POST', body: JSON.stringify({ padding: 'x'.repeat(8192) }) })).status, 413);
  assert.equal((await request('/login/qr', { method: 'POST' })).status, 415);
  for (const value of ['-1', '0', '1.5', '9007199254740992', '1e3', '']) {
    assert.equal((await request(`/song/url?id=${value}`)).status, 400);
    assert.equal((await request(`/song/detail?id=${value}`)).status, 400);
  }
  assert.equal((await request('/search?keywords=')).status, 400);
  assert.equal((await request('/search?keywords=test&limit=101')).status, 400);
  assert.equal((await request('/song/url?id=1&level=unblock')).status, 400);
  assert.equal(calls.length, 0);
});

test('QR authorization persists cookies privately, status verifies freshly, logout clears session', async (t) => {
  const { credentials, request, calls, directory } = await fixture(t);
  assert.equal((await request('/account/status')).data.loggedIn, false);
  assert.equal(calls.length, 0);
  const qr = await request('/login/qr', { method: 'POST', body: {} });
  assert.equal(qr.data.key, 'private_qr_key');
  assert.equal(qr.data.qrImage.startsWith('data:image/png;base64,'), true);
  const check = await request('/login/qr/check', { method: 'POST', body: { key: qr.data.key } });
  assert.equal(check.data.code, 803);
  assert.equal(JSON.stringify(check.data).includes('private_account_cookie'), false);
  assert.equal(JSON.stringify(check.data).includes('cookie'), false);
  assert.equal(credentials.cookie, 'MUSIC_U=private_account_cookie; __csrf=csrf_value');
  assert.equal((await fs.stat(path.join(directory, 'netease'))).mode & 0o777, 0o700);
  for (const filename of ['cookie.json', 'token']) {
    assert.equal((await fs.stat(path.join(directory, 'netease', filename))).mode & 0o777, 0o600);
  }
  const restored = await openCredentials(directory);
  assert.equal(restored.token, credentials.token);
  assert.equal(restored.cookie, credentials.cookie);
  assert.equal((await request('/account/status')).data.nickname, '测试账号');
  assert.equal((await request('/account/status')).data.vipType, 11);
  assert.equal(calls.filter((call) => call.name === 'login_status').length, 2);
  const loggedOut = await request('/logout', { method: 'POST', body: {} });
  assert.equal(loggedOut.data.loggedIn, false);
  assert.equal(credentials.cookie, '');
  assert.equal((await openCredentials(directory)).cookie, '');
  assert.equal((await fs.readdir(path.join(directory, 'netease'))).some((name) => name.endsWith('.tmp')), false);
});

test('search, trial playback and lyrics preserve account rights and 64 bit IDs', async (t) => {
  const { request, credentials, calls } = await fixture(t);
  await credentials.saveCookie('MUSIC_U=authorized_only');
  const search = await request('/search?keywords=test&limit=30&offset=0&proxy=bad');
  assert.deepEqual(search.data, { songs: [{ id: TRACK_ID, title: '测试歌曲', artist: '测试歌手', artists: [], album: '测试专辑', coverUrl: 'https://p3.music.126.net/album.jpg', durationMs: 210000, fee: 1 }], total: 1 });
  const playback = await request(`/song/url?id=${TRACK_ID}&level=standard`);
  assert.equal(playback.data.id, TRACK_ID);
  assert.equal(playback.data.trial, true);
  assert.equal(playback.data.trialStartMs, 30000);
  assert.equal(playback.data.trialEndMs, 60000);
  assert.equal(playback.data.durationMs, 210000);
  assert.equal(playback.data.level, 'standard');
  assert.equal((await request(`/lyrics?id=${TRACK_ID}`)).data.lyric, '[00:01.00]测试歌词');
  assert.deepEqual(calls.map((call) => call.name), ['cloudsearch', 'song_url_v1', 'lyric']);
  assert.equal(calls.every((call) => call.query.cookie === 'MUSIC_U=authorized_only'), true);
  assert.equal(calls.some((call) => 'proxy' in call.query), false);
  assert.equal(calls[1].query.level, 'standard');
});

test('new lyrics expose real YRC timing and preserve compatible LRC without credential fields', async (t) => {
  const yrc = '[1000,2000](1000,500,0)逐(1700,1000,0)字歌词';
  const { request, calls, credentials } = await fixture(t, {
    lyric_new: { body: { code: 200, lrc: { lyric: '[00:01]逐字歌词' },
      yrc: { lyric: yrc }, cookie: 'private_upstream_cookie', debug: 'secret' } },
  });
  await credentials.saveCookie('MUSIC_U=lyrics_owner');
  const result = await request(`/lyrics?id=${TRACK_ID}&proxy=untrusted`);
  assert.equal(result.status, 200);
  assert.deepEqual(result.data, { lyric: '[00:01]逐字歌词', yrc });
  assert.deepEqual(calls, [{ name: 'lyric_new', query: { id: TRACK_ID, cookie: 'MUSIC_U=lyrics_owner' } }]);
  assert.equal((await request(`/lyrics?id=${TRACK_ID}`, { authenticated: false })).status, 401);
  assert.equal(calls.length, 1);
});

test('new lyrics fall back once on unavailable provider but never on timeout', async (t) => {
  const { request, calls } = await fixture(t, {
    lyric_new: () => { throw new Error('secret headers must not escape'); },
  });
  const result = await request(`/lyrics?id=${TRACK_ID}`);
  assert.equal(result.status, 200);
  assert.deepEqual(result.data, { lyric: '[00:01.00]测试歌词', yrc: '' });
  assert.deepEqual(calls.map((call) => call.name), ['lyric_new', 'lyric']);

  const timed = await fixture(t, { lyric_new: () => new Promise(() => {}) }, { timeoutMs: 20 });
  assert.equal((await timed.request('/lyrics?id=1')).status, 504);
  assert.deepEqual(timed.calls.map((call) => call.name), ['lyric_new']);
});

test('new lyrics without word timing stay ordinary and lyric payloads are bounded strings', async (t) => {
  let body = { code: 200, lrc: { lyric: '[00:01]逐行歌词' } };
  const { request, calls } = await fixture(t, { lyric_new: () => ({ body }) });
  assert.deepEqual((await request('/lyrics?id=1')).data,
    { lyric: '[00:01]逐行歌词', yrc: '' });
  body = { code: 200, lrc: { lyric: { private: 'no coercion' } }, yrc: { lyric: '字'.repeat(600000) } };
  const bounded = (await request('/lyrics?id=1')).data;
  assert.equal(bounded.lyric, '');
  assert.equal(bounded.yrc.length, 512 * 1024);
  assert.equal(calls.every((call) => call.name === 'lyric_new'), true);
});

test('song detail is available without music login and exposes only requested public metadata', async (t) => {
  const { request, credentials, calls } = await fixture(t);
  assert.equal(credentials.cookie, '');
  const detail = await request(`/song/detail?id=${TRACK_ID}`);
  assert.equal(detail.status, 200);
  assert.deepEqual(detail.data, { song: {
    id: TRACK_ID, title: '测试歌曲', artist: '测试歌手', artists: [], album: '测试专辑',
    coverUrl: 'https://p3.music.126.net/album.jpg', durationMs: 210000, fee: 1,
  } });
  assert.deepEqual(calls, [{ name: 'song_detail', query: { ids: String(TRACK_ID), cookie: '' } }]);
  assert.equal(JSON.stringify(detail.data).includes('provider_secret'), false);
  assert.equal((await request(`/song/detail?id=${TRACK_ID}`, { authenticated: false })).status, 401);
});

test('playlist artist ID filter accepts aliases and collaborations but rejects same-name impostors', async (t) => {
  const artistId = 7763;
  const make = (id, name, credits) => ({ id, name, ar: credits, dt: 240000 });
  const { request, calls } = await fixture(t, { playlist_detail: { body: { code: 200,
    playlist: { id: PLAYLIST_ID, name: '散步', tags: ['散步'], tracks: [
      make(TRACK_ID, '真实歌曲', [{ id: artistId, name: 'G.E.M.邓紫棋' }]),
      make(TRACK_ID + 1, '合唱', [{ id: 1, name: '合作歌手' }, { id: artistId, name: 'G.E.M.邓紫棋' }]),
      make(TRACK_ID + 2, '邓紫棋翻唱', [{ id: 2, name: '邓紫棋' }]),
      make(TRACK_ID + 3, '署名缺失', [{ name: '邓紫棋' }]),
    ] },
  } } });
  const response = await request(`/radio/playlist?id=${PLAYLIST_ID}&artistId=${artistId}`);
  assert.equal(response.status, 200);
  assert.deepEqual(response.data.songs.map((song) => song.id), [TRACK_ID, TRACK_ID + 1]);
  assert.equal((await request(`/radio/playlist?id=${PLAYLIST_ID}&artistId=0`)).status, 400);
  assert.equal((await request(`/radio/playlist?id=${PLAYLIST_ID}&artistId=${artistId}&artist=邓紫棋`)).status, 400);
  assert.equal(calls.length, 1);
});

test('song detail never substitutes a different song or malformed upstream result', async (t) => {
  const { request } = await fixture(t, { song_detail: { body: {
    code: 200, songs: [{ id: TRACK_ID + 1, name: '不匹配的歌曲' }],
  } } });
  const result = await request(`/song/detail?id=${TRACK_ID}`);
  assert.equal(result.status, 404);
  assert.equal(result.data.error.code, 'song_not_found');
  assert.equal(JSON.stringify(result.data).includes('不匹配'), false);
});

test('missing song detail and failed upstream responses are explicit', async (t) => {
  let response = { body: { code: 200, songs: [] } };
  const { request } = await fixture(t, { song_detail: () => response });
  assert.equal((await request(`/song/detail?id=${TRACK_ID}`)).status, 404);
  for (const body of [{ code: 500, songs: [] }, { code: 200 }, { code: 200, songs: {} }]) {
    response = { body };
    const result = await request(`/song/detail?id=${TRACK_ID}`);
    assert.equal(result.status, 502);
    assert.equal(result.data.error.code, 'invalid_upstream_response');
  }
});

test('album metadata supports old field names and rejects unrelated artwork URLs', async (t) => {
  const valid = 'https://p4.music.126.net/cover%20art.jpg?param=160y160';
  const values = [
    ['http://p1.music.126.net/cover.jpg', 'https://p1.music.126.net/cover.jpg'],
    ['http://p2.music.126.net:80/cover.jpg', 'https://p2.music.126.net/cover.jpg'],
    [valid, valid],
    ['https://p3.music.126.net.evil.example/cover.jpg', ''],
    ['https://fakemusic.126.net/cover.jpg', ''],
    ['https://p3.music.126.net@evil.example/cover.jpg', ''],
    ['https://user:secret@p3.music.126.net/cover.jpg', ''],
    ['https://p3.music.126.net:8443/cover.jpg', ''],
    ['http://127.0.0.1/cover.jpg', ''],
    ['file:///etc/passwd', ''],
    ['data:image/png;base64,dGVzdA==', ''],
    ['https:p3.music.126.net/cover.jpg', ''],
    ['https://p3.music.126.net/cover.jpg#fragment', ''],
    ['https://p3.music.126.net/cover.jpg\\invalid', ''],
    [`https://p3.music.126.net/${'x'.repeat(2048)}`, ''],
  ];
  const { request } = await fixture(t, { cloudsearch: { body: { code: 200, result: {
    songCount: values.length,
    songs: values.map(([picUrl], index) => ({ id: TRACK_ID + index, name: '歌曲',
      artists: [{ name: '歌手' }], album: { name: '旧版专辑', picUrl }, duration: 123000 })),
  } } } });
  const result = await request('/search?keywords=test');
  assert.equal(result.status, 200);
  assert.deepEqual(result.data.songs.map((song) => song.coverUrl), values.map(([, expected]) => expected));
  assert.equal(result.data.songs.every((song) => song.album === '旧版专辑' && song.artist === '歌手'), true);
});

test('search metadata handles malformed artists and bounds upstream results and text', async (t) => {
  const { request } = await fixture(t, { cloudsearch: { body: { code: 200, result: {
    songCount: 3, songs: [
      { id: TRACK_ID, name: '歌'.repeat(1000), ar: {}, al: { name: '辑'.repeat(1000), picUrl: 'https://evil.example/cover' } },
      { id: TRACK_ID + 1, name: '额外的歌曲' },
      null,
    ],
  } } } });
  const result = await request('/search?keywords=test&limit=1');
  assert.equal(result.status, 200);
  assert.equal(result.data.songs.length, 1);
  assert.equal(result.data.songs[0].title.length, 500);
  assert.equal(result.data.songs[0].album.length, 500);
  assert.equal(result.data.songs[0].artist, '');
  assert.equal(result.data.songs[0].coverUrl, '');
});

test('radio search uses playlist search and permits an anonymous music account', async (t) => {
  const { request, credentials, calls } = await fixture(t, { cloudsearch: { body: { code: 200, result: {
    playlistCount: 123, playlists: [{ id: PLAYLIST_ID, name: '雨天轻音乐',
      coverImgUrl: 'http://p4.music.126.net/playlist.jpg', trackCount: 88,
      creator: { nickname: 'not exposed' }, cookie: 'not exposed' }],
  } } } });
  assert.equal(credentials.cookie, '');
  const response = await request('/radio/search?keywords=%20雨天%20&limit=2&offset=5&proxy=bad');
  assert.equal(response.status, 200);
  assert.deepEqual(response.data, { playlists: [{ id: PLAYLIST_ID, name: '雨天轻音乐',
    coverUrl: 'https://p4.music.126.net/playlist.jpg', trackCount: 88 }], total: 123 });
  assert.deepEqual(calls, [{ name: 'cloudsearch', query: {
    keywords: '雨天', limit: 2, offset: 5, type: 1000, cookie: '',
  } }]);
  assert.equal((await request('/radio/search?keywords=雨天', { authenticated: false })).status, 401);
});

test('radio search validates bounded keywords and pagination before calling upstream', async (t) => {
  const { request, calls } = await fixture(t);
  for (const route of [
    '/radio/search', '/radio/search?keywords=', '/radio/search?keywords=%20%20',
    `/radio/search?keywords=${'a'.repeat(201)}`,
    ...['0', '6', '-1', '1.5', '1e1', '', '9007199254740992'].map((value) =>
      `/radio/search?keywords=test&limit=${value}`),
    ...['-1', '1001', '1.5', '1e1', '', '9007199254740992'].map((value) =>
      `/radio/search?keywords=test&offset=${value}`),
  ]) {
    assert.equal((await request(route)).status, 400, route);
  }
  assert.equal(calls.length, 0);
});

test('radio search bounds and sanitizes playlist metadata and uses safe image URLs', async (t) => {
  const { request, calls, credentials } = await fixture(t, { cloudsearch: { body: { code: 200, result: {
    playlistCount: 8, playlists: [
      { id: PLAYLIST_ID, name: '单'.repeat(300), trackCount: 50,
        coverImgUrl: 'https://p4.music.126.net.evil.example/cover.jpg' },
      { id: PLAYLIST_ID + 1, name: '超出本页', trackCount: 20 },
    ],
  } } } });
  await credentials.saveCookie('MUSIC_U=radio_account');
  const response = await request('/radio/search?keywords=test&limit=1&offset=1000');
  assert.equal(response.status, 200);
  assert.equal(response.data.playlists.length, 1);
  assert.equal(response.data.playlists[0].name.length, 160);
  assert.equal(response.data.playlists[0].coverUrl, '');
  assert.equal(calls[0].query.cookie, 'MUSIC_U=radio_account');
  assert.equal(calls[0].query.offset, 1000);
});

test('radio search handles empty results and rejects malformed upstream collections', async (t) => {
  let body = { code: 200, result: { playlistCount: 0 } };
  const { request } = await fixture(t, { cloudsearch: () => ({ body }) });
  assert.deepEqual((await request('/radio/search?keywords=test')).data, { playlists: [], total: 0 });
  body = { code: 200, result: { playlistCount: 0, playlists: [] } };
  assert.deepEqual((await request('/radio/search?keywords=test')).data, { playlists: [], total: 0 });
  for (const malformed of [{ code: 500 }, { code: 200 },
    { code: 200, result: { playlistCount: 1 } }, { code: 200, result: { playlists: {} } }]) {
    body = malformed;
    const response = await request('/radio/search?keywords=test');
    assert.equal(response.status, 502);
    assert.equal(response.data.error.code, 'invalid_upstream_response');
  }
  body = { code: 200, result: { playlistCount: 4, playlists: [
    null, { id: '123', name: '字符串ID' }, { id: -1, name: '负数ID' },
    { id: PLAYLIST_ID, trackCount: -1, coverImgUrl: 'file:///private/cover' },
  ] } };
  assert.deepEqual((await request('/radio/search?keywords=test')).data, {
    playlists: [{ id: PLAYLIST_ID, name: '', coverUrl: '', trackCount: 0 }], total: 4,
  });
});

test('radio playlist returns only bounded public song metadata without asserting playback rights', async (t) => {
  const { request, credentials, calls } = await fixture(t);
  assert.equal(credentials.cookie, '');
  const response = await request(`/radio/playlist?id=${PLAYLIST_ID}&limit=20&proxy=bad`);
  assert.equal(response.status, 200);
  assert.deepEqual(response.data, {
    playlist: { id: PLAYLIST_ID, name: '雨天轻音乐', coverUrl: 'https://p4.music.126.net/playlist.jpg',
      tags: ['轻音乐', '学习'], description: '适合雨天的歌单。' },
    songs: [{ id: TRACK_ID, title: '测试歌曲', artist: '测试歌手', artists: [], album: '测试专辑',
      coverUrl: 'https://p3.music.126.net/album.jpg', durationMs: 210000, fee: 1 }],
  });
  assert.deepEqual(calls, [{ name: 'playlist_detail', query: { id: PLAYLIST_ID, cookie: '' } }]);
  assert.equal(Object.hasOwn(response.data.songs[0], 'url'), false);
  assert.equal(Object.hasOwn(response.data.songs[0], 'playable'), false);
  assert.equal((await request(`/radio/playlist?id=${PLAYLIST_ID}`, { authenticated: false })).status, 401);
});

test('radio playlist validates safe IDs and its small track limit before upstream', async (t) => {
  const { request, calls } = await fixture(t);
  for (const id of ['', '0', '-1', '1.5', '1e3', '9007199254740992'])
    assert.equal((await request(`/radio/playlist?id=${id}`)).status, 400, id);
  for (const limit of ['', '0', '-1', '21', '1.5', '1e1'])
    assert.equal((await request(`/radio/playlist?id=${PLAYLIST_ID}&limit=${limit}`)).status, 400, limit);
  assert.equal(calls.length, 0);
});

test('artist playlist filter runs before response cap on existing public metadata without more upstream calls',async(t)=>{
  const make=(id,name)=>({id,name:'真实歌曲',ar:[{name}],al:{name:'专辑'},dt:240000});
  const tracks=[...Array.from({length:62},(_,i)=>make(TRACK_ID+i,'其他歌手')),
    make(TRACK_ID+62,'陶喆'),{...make(TRACK_ID+63,'陶喆'),ar:[{name:'蔡依林'},{name:'陶喆'}]},
    {...make(TRACK_ID+64,'其他歌手'),name:'陶喆翻唱'},make(TRACK_ID+65,'陶喆翻唱者')];
  const {request,calls}=await fixture(t,{playlist_detail:{body:{code:200,playlist:{id:PLAYLIST_ID,name:'陶喆（轻快版）',tracks}}}});
  const response=await request(`/radio/playlist?id=${PLAYLIST_ID}&limit=20&artist=${encodeURIComponent('陶喆')}`);
  assert.equal(response.status,200);assert.deepEqual(response.data.songs.map((t)=>t.id),[TRACK_ID+62,TRACK_ID+63]);
  assert.equal(calls.length,1);assert.equal(calls[0].name,'playlist_detail');
  assert.equal(Object.hasOwn(calls[0].query,'artist'),false);
});

test('artist playlist filter validates before querying and keeps scan bounded',async(t)=>{
  const {request,calls}=await fixture(t,{playlist_detail:{body:{code:200,playlist:{id:PLAYLIST_ID,name:'边界',
    tracks:[...Array.from({length:2000},(_,i)=>({id:TRACK_ID+i,name:'歌曲',ar:[{name:'其他歌手'}]})),
      {id:TRACK_ID+2000,name:'歌曲',ar:[{name:'陶喆'}]}]}}}});
  assert.equal((await request(`/radio/playlist?id=${PLAYLIST_ID}&artist=`)).status,400);
  assert.equal((await request(`/radio/playlist?id=${PLAYLIST_ID}&artist=${'a'.repeat(101)}`)).status,400);
  assert.equal(calls.length,0);
  const response=await request(`/radio/playlist?id=${PLAYLIST_ID}&artist=${encodeURIComponent('陶喆')}`);
  assert.equal(response.status,200);assert.deepEqual(response.data.songs,[]);assert.equal(calls.length,1);
});

test('radio playlist never substitutes a different playlist and rejects missing tracks', async (t) => {
  let body;
  const { request, calls } = await fixture(t, { playlist_detail: () => ({ body }) });
  for (const malformed of [
    { code: 500 }, { code: 200 }, { code: 200, playlist: null },
    { code: 200, playlist: { id: PLAYLIST_ID + 1, tracks: [] } },
    { code: 200, playlist: { id: String(PLAYLIST_ID), tracks: [] } },
    { code: 200, playlist: { id: PLAYLIST_ID } },
    { code: 200, playlist: { id: PLAYLIST_ID, tracks: {} } },
  ]) {
    body = malformed;
    const response = await request(`/radio/playlist?id=${PLAYLIST_ID}`);
    assert.equal(response.status, 502);
    assert.equal(response.data.error.code, 'invalid_upstream_response');
  }
  assert.equal(calls.every((call) => call.name === 'playlist_detail'), true);
});

test('radio playlist permits empty or partial tracks without fetching the remaining IDs', async (t) => {
  let playlist = { id: PLAYLIST_ID, name: '空歌单', tracks: [], trackIds: [{ id: TRACK_ID }] };
  const { request, calls } = await fixture(t, { playlist_detail: () => ({ body: { code: 200, playlist } }) });
  assert.deepEqual((await request(`/radio/playlist?id=${PLAYLIST_ID}`)).data, {
    playlist: { id: PLAYLIST_ID, name: '空歌单', coverUrl: '', tags: [], description: '' }, songs: [],
  });
  playlist = { id: PLAYLIST_ID, tracks: [null, { id: 'bad' },
    { id: TRACK_ID, name: '旧结构歌曲', artists: [{ name: '歌手' }],
      album: { name: '专辑', picUrl: 'https://p1.music.126.net/cover.jpg' }, duration: 100000 }],
    trackIds: Array.from({ length: 10000 }, (_, index) => ({ id: TRACK_ID + index })) };
  const partial = await request(`/radio/playlist?id=${PLAYLIST_ID}`);
  assert.equal(partial.status, 200);
  assert.equal(partial.data.songs.length, 1);
  assert.equal(partial.data.songs[0].artist, '歌手');
  assert.equal(partial.data.songs[0].coverUrl, 'https://p1.music.126.net/cover.jpg');
  assert.deepEqual(calls.map((call) => call.name), ['playlist_detail', 'playlist_detail']);
});

test('radio playlist caps songs, names, description and tags before returning to Qt', async (t) => {
  const { request, calls, credentials } = await fixture(t, { playlist_detail: { body: { code: 200, playlist: {
    id: PLAYLIST_ID, name: '名'.repeat(300), description: '说'.repeat(1000),
    coverImgUrl: 'https://user:secret@p3.music.126.net/cover.jpg',
    tags: ['标'.repeat(100), '学习', '轻音乐', '休息', '雨天', '夜晚', '不应出现'],
    tracks: Array.from({ length: 100 }, (_, index) => ({ id: TRACK_ID + index, name: '歌曲',
      ar: [{ name: '歌手' }], al: { picUrl: 'https://evil.example/cover' } })),
  } } } });
  await credentials.saveCookie('MUSIC_U=radio_account');
  const response = await request(`/radio/playlist?id=${PLAYLIST_ID}`);
  assert.equal(response.status, 200);
  assert.equal(response.data.songs.length, 20);
  assert.equal(response.data.songs.at(-1).id, TRACK_ID + 19);
  assert.equal(response.data.playlist.name.length, 160);
  assert.equal(response.data.playlist.description.length, 500);
  assert.equal(response.data.playlist.tags.length, 6);
  assert.equal(response.data.playlist.tags[0].length, 40);
  assert.equal(response.data.playlist.coverUrl, '');
  assert.equal(response.data.songs.every((song) => song.coverUrl === ''), true);
  assert.deepEqual(calls, [{ name: 'playlist_detail', query: { id: PLAYLIST_ID, cookie: 'MUSIC_U=radio_account' } }]);
  assert.equal((await request(`/radio/playlist?id=${PLAYLIST_ID}&limit=1`)).data.songs.length, 1);
});

test('radio routes retain the loopback, origin, upstream timeout and redaction boundary', async (t) => {
  const { request, calls, server, credentials } = await fixture(t, {
    cloudsearch: () => { throw new Error('MUSIC_U=private_radio_cookie'); },
    playlist_detail: () => new Promise(() => {}),
  }, { timeoutMs: 30 });
  assert.equal((await request('/radio/search?keywords=test', {
    headers: { Origin: 'https://attacker.example' },
  })).status, 403);
  const invalidHost = await new Promise((resolve, reject) => {
    http.get({ hostname: '127.0.0.1', port: server.address().port,
      path: `/radio/playlist?id=${PLAYLIST_ID}`,
      headers: { Host: 'attacker.example', 'X-MusicPlayer-Token': credentials.token } }, (response) => {
      response.resume();
      resolve(response.statusCode);
    }).on('error', reject);
  });
  assert.equal(invalidHost, 403);
  assert.equal(calls.length, 0);
  const failed = await request('/radio/search?keywords=test');
  assert.equal(failed.status, 502);
  assert.equal(JSON.stringify(failed).includes('private_radio_cookie'), false);
  const timedOut = await request(`/radio/playlist?id=${PLAYLIST_ID}`);
  assert.equal(timedOut.status, 504);
  assert.equal(timedOut.data.error.code, 'upstream_timeout');
});

test('unavailable song is explicit and never triggers alternative providers or quality requests', async (t) => {
  const { request, calls } = await fixture(t, {
    song_url_v1: { body: { data: [{ id: TRACK_ID, url: null, level: 'standard', freeTrialInfo: null }] } },
  });
  const playback = await request(`/song/url?id=${TRACK_ID}`);
  assert.equal(playback.status, 200);
  assert.equal(playback.data.url, '');
  assert.equal(playback.data.trial, false);
  assert.equal(playback.data.message.includes('暂无'), true);
  assert.equal(calls.length, 1);
});

test('only the known NetEase audio CDN HTTP scheme is upgraded, preserving signed URLs exactly', () => {
  const normalize = (url) => normalizeSongUrl({ data: [{ id: TRACK_ID, url }] }, TRACK_ID, 'standard').url;
  const suffix = '/signed%2Fpath/Aa%20Bb.mp3?auth=a%2Bb%3D&empty=&ts=123#fragment';
  assert.equal(normalize(`http://m701.music.126.net${suffix}`), `https://m701.music.126.net${suffix}`);
  assert.equal(normalize(`http://music.126.net${suffix}`), `https://music.126.net${suffix}`);
  assert.equal(normalize(`HTTP://M701.MUSIC.126.NET${suffix}`), `https://M701.MUSIC.126.NET${suffix}`);
  assert.equal(normalize(`https://m701.music.126.net${suffix}`), `https://m701.music.126.net${suffix}`);
  for (const host of ['audio.example.com', 'music.126.net.example.com', 'fakemusic.126.net', 'music.126.net@evil.example']) {
    const url = `http://${host}${suffix}`;
    assert.equal(normalize(url), url);
  }
  assert.equal(normalize('http://'), '');
});

test('upstream errors and timeout never expose cookies, headers or QR keys', async (t) => {
  const { request } = await fixture(t, {
    cloudsearch: () => { throw new Error('MUSIC_U=secret; key=private_qr_key'); },
    lyric: () => new Promise(() => {}),
  }, { timeoutMs: 30 });
  const failed = await request('/search?keywords=test');
  assert.equal(failed.status, 502);
  assert.equal(JSON.stringify(failed).includes('secret'), false);
  assert.equal(JSON.stringify(failed).includes('private_qr_key'), false);
  const timedOut = await request('/lyrics?id=1');
  assert.equal(timedOut.status, 504);
  assert.equal(timedOut.data.error.code, 'upstream_timeout');
});

test('logout invalidates an in-flight QR authorization', async (t) => {
  let complete;
  const pending = new Promise((resolve) => { complete = resolve; });
  const { request, credentials, calls } = await fixture(t, { login_qr_check: () => pending });
  const qr = await request('/login/qr', { method: 'POST', body: {} });
  const check = request('/login/qr/check', { method: 'POST', body: { key: qr.data.key } });
  while (!calls.some((call) => call.name === 'login_qr_check')) await new Promise((resolve) => setImmediate(resolve));
  await request('/logout', { method: 'POST', body: {} });
  complete({ body: { code: 803 }, cookie: ['MUSIC_U=stale_cookie'] });
  assert.equal((await check).data.code, 800);
  assert.equal(credentials.cookie, '');
});

test('an in-flight account status cannot restore a logged-out profile in the UI', async (t) => {
  let complete;
  const pending = new Promise(resolve => { complete = resolve; });
  const { request, credentials, calls } = await fixture(t, { login_status: () => pending });
  const accountA = await credentials.forProfile(PROFILE_A);
  await accountA.saveCookie('MUSIC_U=account_a_fixture');
  const status = request('/account/status', { headers: profileHeaders(PROFILE_A) });
  while (!calls.some(call => call.name === 'login_status')) await new Promise(resolve => setImmediate(resolve));
  await request('/logout', { method: 'POST', body: {}, headers: profileHeaders(PROFILE_A) });
  complete({ body: { data: { code: 200, account: { id: 123 }, profile: { nickname: 'stale-profile', vipType: 11 } } } });
  assert.equal((await status).data.loggedIn, false);
  assert.equal(accountA.cookie, '');
});

test('concurrent credential save then logout leaves no cookie or temporary files', async (t) => {
  const { credentials, directory } = await fixture(t);
  await Promise.all([credentials.saveCookie('MUSIC_U=old'), credentials.clearCookie()]);
  assert.equal(credentials.cookie, '');
  assert.equal((await openCredentials(directory)).cookie, '');
  assert.deepEqual(await fs.readdir(path.join(directory, 'netease')), ['token']);
});

const PROFILE_A = 'a'.repeat(64);
const PROFILE_B = 'b'.repeat(64);
const profileHeaders = (value) => ({ 'X-MusicPlayer-Profile': value });

test('legacy guest cookie is never inherited by a Tango profile and profile slots restore independently', async (t) => {
  const { credentials, request, directory, calls } = await fixture(t);
  await credentials.saveCookie('MUSIC_U=guest_fixture');
  assert.equal((await request('/account/status')).data.loggedIn, true);
  assert.equal((await request('/account/status', { headers: profileHeaders(PROFILE_A) })).data.loggedIn, false);
  const accountA = await credentials.forProfile(PROFILE_A);
  const accountB = await credentials.forProfile(PROFILE_B);
  await accountA.saveCookie('MUSIC_U=account_a_fixture');
  await accountB.saveCookie('MUSIC_U=account_b_fixture');
  await request('/search?keywords=test', { headers: profileHeaders(PROFILE_A) });
  await request('/search?keywords=test', { headers: profileHeaders(PROFILE_B) });
  assert.deepEqual(calls.filter(call => call.name === 'cloudsearch').map(call => call.query.cookie),
    ['MUSIC_U=account_a_fixture', 'MUSIC_U=account_b_fixture']);
  const restored = await openCredentials(directory);
  assert.equal(restored.cookie, 'MUSIC_U=guest_fixture');
  assert.equal((await restored.forProfile(PROFILE_A)).cookie, 'MUSIC_U=account_a_fixture');
  assert.equal((await restored.forProfile(PROFILE_B)).cookie, 'MUSIC_U=account_b_fixture');
  for (const profile of [PROFILE_A, PROFILE_B]) {
    const root = path.join(directory, 'netease', 'profiles', profile);
    assert.equal((await fs.stat(root)).mode & 0o777, 0o700);
    assert.equal((await fs.stat(path.join(root, 'cookie.json'))).mode & 0o777, 0o600);
  }
});

test('profile logout only clears its own connection, preserving another account and legacy guest', async (t) => {
  const { credentials, request, calls } = await fixture(t);
  await credentials.saveCookie('MUSIC_U=guest_fixture');
  const accountA = await credentials.forProfile(PROFILE_A);
  const accountB = await credentials.forProfile(PROFILE_B);
  await accountA.saveCookie('MUSIC_U=account_a_fixture');
  await accountB.saveCookie('MUSIC_U=account_b_fixture');
  assert.equal((await request('/logout', { method: 'POST', body: {}, headers: profileHeaders(PROFILE_B) })).status, 200);
  assert.equal(accountB.cookie, '');
  assert.equal(accountA.cookie, 'MUSIC_U=account_a_fixture');
  assert.equal(credentials.cookie, 'MUSIC_U=guest_fixture');
  assert.deepEqual(calls.filter(call => call.name === 'logout').map(call => call.query.cookie), ['MUSIC_U=account_b_fixture']);
});

test('a QR completion remains in its initiating profile when another profile logs out', async (t) => {
  let complete;
  const pending = new Promise(resolve => { complete = resolve; });
  const { request, credentials, calls } = await fixture(t, { login_qr_check: () => pending });
  const qr = await request('/login/qr', { method: 'POST', body: {}, headers: profileHeaders(PROFILE_A) });
  assert.equal((await request('/login/qr/check', { method: 'POST', body: { key: qr.data.key },
    headers: profileHeaders(PROFILE_B) })).data.code, 800);
  const check = request('/login/qr/check', { method: 'POST', body: { key: qr.data.key }, headers: profileHeaders(PROFILE_A) });
  while (!calls.some(call => call.name === 'login_qr_check')) await new Promise(resolve => setImmediate(resolve));
  await request('/logout', { method: 'POST', body: {}, headers: profileHeaders(PROFILE_B) });
  complete({ body: { code: 803 }, cookie: ['MUSIC_U=only_account_a_fixture'] });
  assert.equal((await check).data.code, 803);
  assert.equal((await credentials.forProfile(PROFILE_A)).cookie, 'MUSIC_U=only_account_a_fixture');
  assert.equal((await credentials.forProfile(PROFILE_B)).cookie, '');
  assert.equal(credentials.cookie, '');
});

test('logout invalidates only its own pending QR and does not cancel another profile QR', async (t) => {
  let key = 0;
  const { request, credentials } = await fixture(t, {
    login_qr_key: () => ({ body: { data: { unikey: `fixture_key_${++key}` } } }),
  });
  const qrA = await request('/login/qr', { method: 'POST', body: {}, headers: profileHeaders(PROFILE_A) });
  const qrB = await request('/login/qr', { method: 'POST', body: {}, headers: profileHeaders(PROFILE_B) });
  await request('/logout', { method: 'POST', body: {}, headers: profileHeaders(PROFILE_A) });
  assert.equal((await request('/login/qr/check', { method: 'POST', body: { key: qrA.data.key },
    headers: profileHeaders(PROFILE_A) })).data.code, 800);
  assert.equal((await request('/login/qr/check', { method: 'POST', body: { key: qrB.data.key },
    headers: profileHeaders(PROFILE_B) })).data.code, 803);
  assert.equal((await credentials.forProfile(PROFILE_A)).cookie, '');
  assert.equal((await credentials.forProfile(PROFILE_B)).cookie.includes('private_account_cookie'), true);
});

test('profile headers accept only opaque local slots, never raw account IDs or paths', async (t) => {
  const { request, calls, credentials } = await fixture(t);
  for (const profile of ['../cookie.json', 'server:user-id', 'a'.repeat(63), 'A'.repeat(64), 'guest, ' + PROFILE_A]) {
    assert.equal((await request('/search?keywords=test', { headers: profileHeaders(profile) })).status, 400, profile);
  }
  await assert.rejects(credentials.forProfile('../cookie.json'), /invalid_profile/);
  assert.equal(calls.length, 0);
  assert.equal((await request('/account/status', { headers: profileHeaders('guest') })).status, 200);
});

test('concurrent writes and logout are serialized per profile, without clearing other slots', async (t) => {
  const { credentials, directory } = await fixture(t);
  const [one, two] = await Promise.all([credentials.forProfile(PROFILE_A), credentials.forProfile(PROFILE_A)]);
  assert.equal(one, two);
  const accountB = await credentials.forProfile(PROFILE_B);
  await Promise.all([one.saveCookie('MUSIC_U=old_fixture'), one.clearCookie(), accountB.saveCookie('MUSIC_U=b_fixture')]);
  const restored = await openCredentials(directory);
  assert.equal((await restored.forProfile(PROFILE_A)).cookie, '');
  assert.equal((await restored.forProfile(PROFILE_B)).cookie, 'MUSIC_U=b_fixture');
  assert.equal((await fs.readdir(path.join(directory, 'netease', 'profiles', PROFILE_A))).some(name => name.endsWith('.tmp')), false);
});

test('idle loopback service releases its port', async (t) => {
  const { server } = await fixture(t, {}, { idleTimeoutMs: 30 });
  await once(server, 'close');
  assert.equal(server.listening, false);
});
