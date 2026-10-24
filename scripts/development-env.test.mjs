import test from 'node:test';
import assert from 'node:assert/strict';
import { copyFile, mkdtemp, mkdir, rm, access } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';

const run = promisify(execFile);
async function fixture(t) {
  const directory = await mkdtemp(join(tmpdir(), 'tango-dev-shell-')), scripts = join(directory, 'scripts');
  await mkdir(scripts); const file = join(scripts, 'development-env.sh');
  await copyFile(new URL('./development-env.sh', import.meta.url), file);
  t.after(() => rm(directory, { recursive: true, force: true }));
  return { directory, file };
}
async function source(fx, overrides = {}) {
  const result = await run('/bin/bash', ['--noprofile', '--norc', '-c',
    'set -euo pipefail; source "$1"; printf "%s\\n" "${MUSICPLAYER_BACKEND_URL-}" "${MUSICPLAYER_DEVELOPMENT_MODE-}" "${MUSICPLAYER_DEVELOPMENT_TOKEN_FILE-}"', 'test', fx.file],
  { cwd: fx.directory, env: { PATH: '/usr/bin:/bin', ...overrides }, timeout: 2000 });
  const [url, mode, tokenFile] = result.stdout.split('\n'); return { url, mode, tokenFile };
}
test('local script defaults use isolated port 16383 and development mode without reading or creating files', async (t) => {
  const fx = await fixture(t), state = await source(fx);
  assert.deepEqual(state, { url: 'http://127.0.0.1:16383', mode: '1', tokenFile: join(fx.directory, '.local/development/session.token') });
  assert.equal(await access(join(fx.directory, '.local')).then(() => true, () => false), false);
});
test('explicit mode zero selects old port 16382 and keeps registration/login enabled', async (t) => {
  const fx = await fixture(t);
  assert.deepEqual(await source(fx, { MUSICPLAYER_DEVELOPMENT_MODE: '0' }), { url: 'http://127.0.0.1:16382', mode: '0', tokenFile: '' });
  assert.deepEqual(await source(fx, { MUSICPLAYER_DEVELOPMENT_MODE: '0', MUSICPLAYER_BACKEND_URL: 'http://127.0.0.1:17000' }),
    { url: 'http://127.0.0.1:17000', mode: '0', tokenFile: '' });
});
test('remote and hostname backends default to ordinary account mode without assigning a development capability file', async (t) => {
  const fx = await fixture(t);
  for (const url of ['https://music.example.test', 'https://music.example.test/api', 'http://localhost:16382', 'http://192.168.1.3:16382'])
    assert.deepEqual(await source(fx, { MUSICPLAYER_BACKEND_URL: url }), { url, mode: '0', tokenFile: '' });
});
test('explicit loopback URL and private capability path remain unchanged across repeated sourcing', async (t) => {
  const fx = await fixture(t), custom = join(fx.directory, 'custom.token');
  const values = { MUSICPLAYER_BACKEND_URL: 'http://127.0.0.1:17777', MUSICPLAYER_DEVELOPMENT_TOKEN_FILE: custom };
  const expected = { url: values.MUSICPLAYER_BACKEND_URL, mode: '1', tokenFile: custom };
  assert.deepEqual(await source(fx, values), expected);
  assert.deepEqual(await source(fx, { ...values, MUSICPLAYER_DEVELOPMENT_MODE: '1' }), expected);
  assert.equal(await access(custom).then(() => true, () => false), false);
});
