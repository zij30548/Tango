import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';

test('actual adapter loads the artist songs module without external requests or user credentials', async (t) => {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'tango-upstream-modules-'));
  t.after(() => fs.rm(directory, { recursive: true, force: true }));
  // 上游初始化会读取临时目录匿名令牌；子进程只给测试夹具，不接触真实设备文件。
  await fs.writeFile(path.join(directory, 'anonymous_token'), 'isolated-public-test-placeholder', { mode: 0o600 });
  const moduleUrl = new URL('../upstream.mjs', import.meta.url).href;
  const script = `import assert from 'node:assert/strict';
    const {createUpstream}=await import(${JSON.stringify(moduleUrl)});
    const upstream=createUpstream();
    for(const key of ['cloudsearch','artist_songs','artist_album','playlist_detail'])
      assert.equal(typeof upstream[key],'function');
    assert.equal(upstream.api,undefined);`;
  const result = spawnSync(process.execPath, ['--input-type=module', '-e', script], {
    env: { PATH: process.env.PATH, TMPDIR: directory }, encoding: 'utf8', timeout: 15000,
  });
  assert.equal(result.error, undefined);
  assert.equal(result.status, 0, result.stderr);
});
