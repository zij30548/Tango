import assert from 'node:assert/strict';
import test from 'node:test';
import http from 'node:http';
import { once } from 'node:events';
import { createMusicServer } from '../app.mjs';

const ARTIST_ID = 12345678901;
const ALBUM_ID = 22345678901;
const SONG_ID = 32345678901;
const RELEASED_AT = Date.UTC(2026, 0, 1);
const artist = { id: ARTIST_ID, name: '测试歌手', alias: ['艺名'], picUrl: 'http://p1.music.126.net/artist.jpg' };
const album = { id: ALBUM_ID, name: '最新单曲', publishTime: RELEASED_AT, type: 'Single',
  picUrl: 'http://p2.music.126.net/album.jpg', artist };
const song = { id: SONG_ID, name: '新歌', ar: [{ name: '测试歌手' }],
  al: { id: ALBUM_ID, name: '最新单曲', picUrl: 'http://p2.music.126.net/album.jpg' }, dt: 180000, fee: 1 };

async function fixture(t, overrides = {}, options = {}) {
  const credentials = { token: 'test-assistant-local-token', cookie: '' };
  const calls = [];
  const defaults = {
    cloudsearch: { body: { code: 200, result: { artistCount: 1, artists: [artist] } } },
    artist_album: { body: { code: 200, artist, hotAlbums: [album] } },
    album: { body: { code: 200, album, songs: [song] } },
  };
  const upstream = Object.fromEntries(Object.entries({ ...defaults, ...overrides }).map(([name, response]) =>
    [name, async (query) => {
      calls.push({ name, query });
      return typeof response === 'function' ? response(query) : structuredClone(response);
    }]));
  const server = createMusicServer({ upstream, credentials, idleTimeoutMs: 0, ...options });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  t.after(async () => {
    server.closeAllConnections();
    await new Promise((resolve) => server.close(resolve));
  });
  const base = `http://127.0.0.1:${server.address().port}`;
  async function request(route, { authenticated = true, headers = {} } = {}) {
    const response = await fetch(base + route, { headers: {
      ...(authenticated ? { 'X-MusicPlayer-Token': credentials.token } : {}), ...headers,
    } });
    return { status: response.status, headers: response.headers, data: await response.json() };
  }
  return { request, calls, credentials, server };
}

test('assistant artist lookup uses bounded public type-100 search and verified metadata only', async (t) => {
  const { request, calls } = await fixture(t);
  const response = await request('/assistant/artists?keywords=%20测试歌手%20&limit=3&offset=100&proxy=evil');
  assert.equal(response.status, 200);
  assert.deepEqual(response.data, { artists: [{ id: ARTIST_ID, name: '测试歌手', aliases: ['艺名'],
    coverUrl: 'https://p1.music.126.net/artist.jpg' }], total: 1 });
  assert.deepEqual(calls, [{ name: 'cloudsearch', query: {
    keywords: '测试歌手', limit: 3, offset: 0, type: 100, cookie: '',
  } }]);
  assert.equal(response.headers.get('access-control-allow-origin'), null);
});

test('assistant artist lookup validates names and small limits before upstream', async (t) => {
  const { request, calls } = await fixture(t);
  for (const route of [
    '/assistant/artists', '/assistant/artists?keywords=%20%20',
    `/assistant/artists?keywords=${'x'.repeat(201)}`,
    ...['', '0', '6', '-1', '1.1', '1e1', '9007199254740992'].map((limit) =>
      `/assistant/artists?keywords=test&limit=${limit}`),
  ]) assert.equal((await request(route)).status, 400, route);
  assert.equal(calls.length, 0);
});

test('assistant artist metadata is bounded, deduplicated and refuses arbitrary artwork', async (t) => {
  const { request } = await fixture(t, { cloudsearch: { body: { code: 200, result: {
    artistCount: 6, artists: [null, { ...artist, name: '名'.repeat(200),
      alias: ['别'.repeat(200), '短', '短', 'a', 'b', 'c', '不得出现'],
      picUrl: 'https://p1.music.126.net.evil.example/cover', img1v1Url: 'http://127.0.0.1/image',
      cookie: 'not-returned', description: 'not-returned' }, artist,
    { id: ARTIST_ID + 1, name: ' ' }, { id: 'unsafe', name: '无效' },
    { id: ARTIST_ID + 2, name: '超过页数' }],
  } } } });
  const response = await request('/assistant/artists?keywords=test');
  assert.equal(response.data.artists.length, 1);
  assert.equal(response.data.artists[0].name.length, 160);
  assert.equal(response.data.artists[0].aliases[0].length, 100);
  assert.equal(response.data.artists[0].aliases.includes('不得出现'), false);
  assert.equal(response.data.artists[0].coverUrl, '');
  assert.equal(JSON.stringify(response.data).includes('not-returned'), false);
});

test('assistant artist lookup handles no matches and rejects malformed upstream collections', async (t) => {
  let body = { code: 200, result: { artistCount: 0 } };
  const { request } = await fixture(t, { cloudsearch: () => ({ body }) });
  assert.deepEqual((await request('/assistant/artists?keywords=test')).data, { artists: [], total: 0 });
  for (const invalid of [{ code: 500 }, { code: 200 },
    { code: 200, result: { artistCount: 1 } }, { code: 200, result: { artists: {} } }]) {
    body = invalid;
    assert.equal((await request('/assistant/artists?keywords=test')).status, 502);
  }
});

test('artist tracks page is bounded and retains real artist IDs without authorizing playback', async (t) => {
  const { request, calls, credentials } = await fixture(t, { artist_songs: { body: {
    code: 200, total: 50, more: true, songs: [{ ...song,
      ar: [{ id: ARTIST_ID, name: 'G.E.M.邓紫棋' }, { id: ARTIST_ID + 1, name: '合作歌手' }] }],
  } } });
  credentials.cookie = 'MUSIC_U=test-device-only';
  const before = Date.now();
  const response = await request(`/artist/tracks?artistId=${ARTIST_ID}&limit=2&offset=30&order=time&proxy=evil`);
  assert.equal(response.status, 200);
  assert.deepEqual(response.data.songs[0].artists, [{ id: ARTIST_ID, name: 'G.E.M.邓紫棋' },
    { id: ARTIST_ID + 1, name: '合作歌手' }]);
  assert.equal(response.data.songs[0].artist, 'G.E.M.邓紫棋 / 合作歌手');
  assert.equal(response.data.artistId, ARTIST_ID);
  assert.equal(response.data.total, 50);
  assert.equal(response.data.offset, 30);
  assert.equal(response.data.limit, 2);
  assert.equal(response.data.more, true);
  assert.ok(response.data.checkedAt >= before && response.data.checkedAt <= Date.now());
  assert.deepEqual(calls, [{ name: 'artist_songs', query: {
    id: ARTIST_ID, limit: 2, offset: 30, order: 'hot', cookie: 'MUSIC_U=test-device-only',
  } }]);
  assert.equal(Object.hasOwn(response.data.songs[0], 'url'), false);
  assert.equal(Object.hasOwn(response.data.songs[0], 'playable'), false);
});

test('artist tracks refuses namesakes, title impersonation, missing credit IDs and repeated songs', async (t) => {
  const valid = { ...song, ar: [{ id: ARTIST_ID, name: 'G.E.M.邓紫棋' }] };
  const { request } = await fixture(t, { artist_songs: { body: { code: 200, total: 7, more: false, songs: [
    valid, { ...valid, name: '重复' }, { ...song, id: SONG_ID + 1, ar: [{ id: ARTIST_ID + 1, name: 'G.E.M.邓紫棋' }] },
    { ...song, id: SONG_ID + 2, name: '邓紫棋金曲', ar: [{ id: ARTIST_ID + 2, name: '翻唱者' }] },
    { ...song, id: SONG_ID + 3, ar: [{ name: 'G.E.M.邓紫棋' }] },
    { ...song, id: SONG_ID + 4, ar: [{ id: String(ARTIST_ID), name: 'G.E.M.邓紫棋' }] }, null,
  ] } } });
  const response = await request(`/artist/tracks?artistId=${ARTIST_ID}`);
  assert.deepEqual(response.data.songs.map((entry) => entry.id), [SONG_ID]);
  assert.equal(response.data.total, 7);
  assert.equal(response.data.more, false);
});

test('artist tracks validates IDs, pagination, authentication and upstream shape before accepting data', async (t) => {
  let body = { code: 200, total: 0, more: false, songs: [] };
  const { request, calls } = await fixture(t, { artist_songs: () => ({ body }) });
  for (const route of ['/artist/tracks', '/artist/tracks?artistId=0', '/artist/tracks?artistId=1.5',
    `/artist/tracks?artistId=${ARTIST_ID}&limit=31`, `/artist/tracks?artistId=${ARTIST_ID}&limit=0`,
    `/artist/tracks?artistId=${ARTIST_ID}&offset=1001`, `/artist/tracks?artistId=${ARTIST_ID}&offset=-1`])
    assert.equal((await request(route)).status, 400, route);
  assert.equal((await request(`/artist/tracks?artistId=${ARTIST_ID}`, { authenticated: false })).status, 401);
  assert.equal(calls.length, 0);
  for (const invalid of [{ code: 500 }, { code: 200 }, { code: 200, songs: {} },
    { code: 200, songs: [], total: -1 }, { code: 200, songs: [], more: 'true' }]) {
    body = invalid;
    assert.equal((await request(`/artist/tracks?artistId=${ARTIST_ID}`)).status, 502);
  }
  body = { code: 200, total: 0, more: false, songs: [] };
  assert.deepEqual((await request(`/artist/tracks?artistId=${ARTIST_ID}`)).data.songs, []);
});

test('artist tracks keeps unknown total explicit and never invents a next page', async (t) => {
  let body={ code:200, more:false, songs:[] };
  const { request } = await fixture(t, { artist_songs: () => ({ body }) });
  let response=await request(`/artist/tracks?artistId=${ARTIST_ID}`);
  assert.equal(response.data.total,null); assert.equal(response.data.more,false);
  body={ code:200, total:31, songs:[] };
  response=await request(`/artist/tracks?artistId=${ARTIST_ID}&offset=30`);
  assert.equal(response.data.total,31); assert.equal(response.data.more,true);
  body={ code:200, songs:[] };
  assert.equal((await request(`/artist/tracks?artistId=${ARTIST_ID}`)).status,502);
});

test('artist resolve merges explicit translated names without deriving substrings as aliases', async (t) => {
  const { request }=await fixture(t,{ cloudsearch:{ body:{ code:200,result:{ artistCount:1,
    artists:[{...artist,name:'G.E.M.邓紫棋',alias:['G.E.M.'],trans:['邓紫棋'],transNames:['邓紫棋','Gloria']}],
  } } } });
  const response=await request('/assistant/artists?keywords=邓紫棋');
  assert.deepEqual(response.data.artists[0].aliases,['G.E.M.','邓紫棋','Gloria']);
  assert.equal(response.data.artists[0].aliases.includes('紫棋'),false);
});

test('assistant releases preserve identity, sort actual release dates and omit future or invalid dates', async (t) => {
  const older = { ...album, id: ALBUM_ID + 1, name: '旧单曲', publishTime: RELEASED_AT - 86400000 };
  const newer = { ...album, id: ALBUM_ID + 2, name: '新单曲', publishTime: RELEASED_AT + 86400000 };
  const { request, calls } = await fixture(t, { artist_album: { body: { code: 200, artist,
    hotAlbums: [older, { ...album, id: ALBUM_ID + 3, publishTime: Date.now() + 86400000 },
      newer, { ...album, id: ALBUM_ID + 4, publishTime: 0 }, album,
      { ...album, id: ALBUM_ID + 5, publishTime: '1767225600000' }], cookie: 'not-returned',
  } } });
  const before = Date.now();
  const response = await request(`/assistant/releases?artistId=${ARTIST_ID}`);
  const after = Date.now();
  assert.equal(response.status, 200);
  assert.deepEqual(response.data.artist, { id: ARTIST_ID, name: '测试歌手' });
  assert.deepEqual(response.data.albums.map((entry) => entry.id), [newer.id, ALBUM_ID, older.id]);
  assert.equal(response.data.checkedAt >= before && response.data.checkedAt <= after, true);
  assert.equal(response.data.albums.every((entry) => entry.publishTime <= response.data.checkedAt), true);
  assert.deepEqual(calls, [{ name: 'artist_album', query: { id: ARTIST_ID, limit: 6, offset: 0, cookie: '' } }]);
  assert.equal(JSON.stringify(response.data).includes('not-returned'), false);
});

test('assistant releases cap returned rows, metadata and upstream work without expanding catalog', async (t) => {
  const { request, calls, credentials } = await fixture(t, { artist_album: { body: { code: 200,
    artist, hotAlbums: Array.from({ length: 30 }, (_, index) => ({ ...album, id: ALBUM_ID + index,
      name: '名'.repeat(200), type: '型'.repeat(80), picUrl: 'https://evil.example/cover' })), more: true,
  } } });
  credentials.cookie = 'MUSIC_U=test-private-account';
  const response = await request(`/assistant/releases?artistId=${ARTIST_ID}&limit=2&offset=6&proxy=evil`);
  assert.equal(response.data.albums.length, 2);
  assert.equal(response.data.albums.every((entry) => entry.name.length === 160 && entry.type.length === 40 && !entry.coverUrl), true);
  assert.deepEqual(calls, [{ name: 'artist_album', query: {
    id: ARTIST_ID, limit: 2, offset: 0, cookie: 'MUSIC_U=test-private-account',
  } }]);
});

test('assistant release collections never substitute artists and allow a valid empty catalog', async (t) => {
  let body;
  const { request } = await fixture(t, { artist_album: () => ({ body }) });
  for (const invalid of [{ code: 500 }, { code: 200 },
    { code: 200, artist: { ...artist, id: ARTIST_ID + 1 }, hotAlbums: [] },
    { code: 200, artist: { ...artist, id: String(ARTIST_ID) }, hotAlbums: [] },
    { code: 200, artist, hotAlbums: {} }, { code: 200, artist }]) {
    body = invalid;
    const response = await request(`/assistant/releases?artistId=${ARTIST_ID}`);
    assert.equal(response.status, 502);
    assert.equal(response.data.error.code, 'invalid_upstream_response');
  }
  body = { code: 200, artist, hotAlbums: [] };
  assert.deepEqual((await request(`/assistant/releases?artistId=${ARTIST_ID}`)).data.albums, []);
});

test('assistant album exposes actual bounded song metadata and no playback authorization', async (t) => {
  const { request, calls } = await fixture(t);
  const response = await request(`/assistant/album?id=${ALBUM_ID}&limit=1&proxy=evil`);
  assert.equal(response.status, 200);
  assert.deepEqual(response.data, { album: {
    id: ALBUM_ID, name: '最新单曲', publishTime: RELEASED_AT, type: 'Single',
    coverUrl: 'https://p2.music.126.net/album.jpg', artistId: ARTIST_ID, artistName: '测试歌手',
  }, songs: [{ id: SONG_ID, title: '新歌', artist: '测试歌手', artists: [], album: '最新单曲',
    coverUrl: 'https://p2.music.126.net/album.jpg', durationMs: 180000, fee: 1 }] });
  assert.deepEqual(calls, [{ name: 'album', query: { id: ALBUM_ID, cookie: '' } }]);
  assert.equal(Object.hasOwn(response.data.songs[0], 'url'), false);
  assert.equal(Object.hasOwn(response.data.songs[0], 'playable'), false);
});

test('assistant album requires matching IDs, actual dates, artist identity and a song collection', async (t) => {
  let body;
  const { request } = await fixture(t, { album: () => ({ body }) });
  for (const invalid of [{ code: 500 }, { code: 200 },
    { code: 200, album: { ...album, id: ALBUM_ID + 1 }, songs: [] },
    { code: 200, album: { ...album, publishTime: Date.now() + 86400000 }, songs: [] },
    { code: 200, album: { ...album, artist: null }, songs: [] },
    { code: 200, album, songs: {} }, { code: 200, album }]) {
    body = invalid;
    assert.equal((await request(`/assistant/album?id=${ALBUM_ID}`)).status, 502);
  }
  body = { code: 200, album, songs: [] };
  assert.deepEqual((await request(`/assistant/album?id=${ALBUM_ID}`)).data.songs, []);
});

test('assistant album caps tracks, removes duplicate IDs and reuses valid album artwork', async (t) => {
  const { request, calls } = await fixture(t, { album: { body: { code: 200,
    album: { ...album, artist: null, artists: [artist] },
    songs: [{ id: SONG_ID, name: '无专辑封面的歌', artists: [{ name: '测试歌手' }] },
      { id: SONG_ID, name: '重复' }, null,
      ...Array.from({ length: 40 }, (_, index) => ({ ...song, id: SONG_ID + index + 1 }))],
  } } });
  const response = await request(`/assistant/album?id=${ALBUM_ID}`);
  assert.equal(response.status, 200);
  assert.equal(response.data.songs.length, 18);
  assert.equal(response.data.songs[0].album, '最新单曲');
  assert.equal(response.data.songs[0].coverUrl, 'https://p2.music.126.net/album.jpg');
  assert.equal(new Set(response.data.songs.map((entry) => entry.id)).size, 18);
  assert.equal(calls.length, 1);
});

test('assistant release and album IDs and limits are validated before any upstream call', async (t) => {
  const { request, calls } = await fixture(t);
  for (const id of ['', '0', '-1', '1.2', '1e3', '9007199254740992']) {
    assert.equal((await request(`/assistant/releases?artistId=${id}`)).status, 400, id);
    assert.equal((await request(`/assistant/album?id=${id}`)).status, 400, id);
  }
  for (const limit of ['', '0', '-1', '1.2', '1e1', '7'])
    assert.equal((await request(`/assistant/releases?artistId=${ARTIST_ID}&limit=${limit}`)).status, 400, limit);
  for (const limit of ['', '0', '-1', '1.2', '1e1', '21'])
    assert.equal((await request(`/assistant/album?id=${ALBUM_ID}&limit=${limit}`)).status, 400, limit);
  assert.equal(calls.length, 0);
});

test('assistant metadata routes retain local token, Host, Origin, timeout and secret-redaction boundaries', async (t) => {
  const { request, calls, server, credentials } = await fixture(t, {
    cloudsearch: () => { throw new Error('MUSIC_U=private-assistant-cookie'); },
    artist_album: () => new Promise(() => {}),
  }, { timeoutMs: 30 });
  for (const route of ['/assistant/artists?keywords=test',
    `/assistant/releases?artistId=${ARTIST_ID}`, `/assistant/album?id=${ALBUM_ID}`]) {
    assert.equal((await request(route, { authenticated: false })).status, 401);
    assert.equal((await request(route, { headers: { Origin: 'https://attacker.example' } })).status, 403);
  }
  const invalidHost = await new Promise((resolve, reject) => {
    http.get({ hostname: '127.0.0.1', port: server.address().port,
      path: `/assistant/album?id=${ALBUM_ID}`,
      headers: { Host: 'attacker.example', 'X-MusicPlayer-Token': credentials.token } }, (response) => {
      response.resume();
      resolve(response.statusCode);
    }).on('error', reject);
  });
  assert.equal(invalidHost, 403);
  assert.equal(calls.length, 0);
  const error = await request('/assistant/artists?keywords=test');
  assert.equal(error.status, 502);
  assert.equal(JSON.stringify(error).includes('private-assistant-cookie'), false);
  assert.equal((await request(`/assistant/releases?artistId=${ARTIST_ID}`)).status, 504);
});
