// 上游失败可能打印含Cookie的响应；任何上游模块导入前先关闭控制台输出。
for (const name of ['log', 'info', 'warn', 'error', 'debug', 'trace', 'dir']) console[name] = () => {};

import { constants } from 'node:fs';
import fs from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { createHash } from 'node:crypto';
import http from 'node:http';

const ownFile = fileURLToPath(import.meta.url);
const project = path.resolve(path.dirname(ownFile), "../..");
const baseDirectory = path.join(project, ".local/music-requirements-live");
const uid = process.getuid();

function denied(code) {
  // 错误只使用固定代码，不带路径内容、Cookie、令牌或上游正文。
  throw new Error(code);
}

function encode(value) {
  if (Array.isArray(value)) return '[' + value.map(encode).join(',') + ']';
  if (value && typeof value === 'object') return '{' + Object.keys(value).sort()
    .map((key) => JSON.stringify(key) + ':' + encode(value[key])).join(',') + '}';
  return JSON.stringify(value);
}

function sha(value) {
  return createHash('sha256').update(value).digest('hex');
}

async function privateDirectory(directory) {
  // 不追随软链接，也不替其他目录修权限；只接受当前用户已经拥有的私有目录。
  const real = await fs.realpath(directory);
  const details = await fs.lstat(directory);
  if (real !== directory || !details.isDirectory() || details.isSymbolicLink()
      || details.uid !== uid || (details.mode & 0o077)) denied('UNSAFE_PRIVATE_DIRECTORY');
  return directory;
}

async function readPrivateJson(filename) {
  const handle = await fs.open(filename, constants.O_RDONLY | constants.O_NOFOLLOW);
  try {
    const details = await handle.stat();
    if (!details.isFile() || details.nlink !== 1 || details.uid !== uid
        || (details.mode & 0o077) || details.size > 512 * 1024) denied('UNSAFE_PRIVATE_JSON');
    return JSON.parse(await handle.readFile('utf8'));
  } finally {
    await handle.close();
  }
}

async function writeNew(filename, value) {
  const handle = await fs.open(filename, constants.O_WRONLY | constants.O_CREAT
    | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
  try {
    await handle.writeFile(encode(value) + '\n');
    await handle.sync();
  } finally {
    await handle.close();
  }
}

async function absent(filename) {
  try {
    await fs.lstat(filename);
  } catch (error) {
    if (error.code === 'ENOENT') return;
    denied('PRIVATE_PATH_UNAVAILABLE');
  }
  denied('ISOLATED_CREDENTIALS_ALREADY_EXIST');
}

async function health(port) {
  // 只探测自己刚绑定的随机端口，不扫描或复用16381等已有服务。
  return new Promise((resolve, reject) => {
    const request = http.get({ host: '127.0.0.1', port, path: '/health', timeout: 1500 }, (response) => {
      let text = '';
      response.setEncoding('utf8');
      response.on('data', (part) => {
        text += part;
        if (text.length > 4096) request.destroy();
      });
      response.on('end', () => {
        try {
          const data = JSON.parse(text);
          if (response.statusCode !== 200 || data.app !== 'quiet-netease'
              || data.version !== 1 || data.profileIsolation !== 1 || data.catalogVersion !== 2
              || data.pid !== process.pid) denied('OWN_SERVICE_HEALTH_FAILED');
          resolve(true);
        } catch {
          reject(new Error('OWN_SERVICE_HEALTH_FAILED'));
        }
      });
      response.on('error', () => reject(new Error('OWN_SERVICE_HEALTH_FAILED')));
    });
    request.on('timeout', () => request.destroy());
    request.on('error', () => reject(new Error('OWN_SERVICE_HEALTH_FAILED')));
  });
}

async function main() {
  process.umask(0o077);
  if (Number(process.versions.node.split('.')[0]) < 20) denied('NODE_VERSION_UNSUPPORTED');
  const args = process.argv.slice(2);
  if (args.length !== 2 || args[0] !== '--run' || !path.isAbsolute(args[1])) denied('EXPECTED_OWNED_RUN_ARGUMENT');
  const run = path.resolve(args[1]);
  if (path.dirname(run) !== baseDirectory || !/^requirements-\d{8}T\d{6}Z-[a-f0-9]{8}$/.test(path.basename(run))) {
    denied('INVALID_OWNED_RUN_DIRECTORY');
  }
  await privateDirectory(baseDirectory);
  await privateDirectory(run);
  const manifest = await readPrivateJson(path.join(run, 'manifest.json'));
  const authorization = await readPrivateJson(path.join(run, 'authorization.json'));
  const connection = await readPrivateJson(path.join(run, 'connection.json'));
  const { seal, ...unsigned } = manifest;
  if (!/^[a-f0-9]{64}$/.test(seal || '') || sha(encode(unsigned)) !== seal
      || connection.manifestSeal !== seal || manifest.purpose !== 'actual_qt_music_requirements_acceptance'
      || authorization.manifestSeal !== seal || authorization.newAuthorization !== true
      || !Number.isSafeInteger(authorization.maximumProviderRequests)
      || authorization.maximumProviderRequests < 1 || authorization.maximumProviderRequests > 80
      || connection.dataDir !== path.join(run, 'qt-data')) denied('RUN_MANIFEST_MISMATCH');
  const ownRelative = path.relative(project, ownFile).split(path.sep).join('/');
  if (manifest.sources?.[ownRelative] !== sha(await fs.readFile(ownFile))) denied('FROZEN_WRAPPER_CHANGED');
  const dataDirectory = await privateDirectory(connection.dataDir);
  // 只接受新隔离目录；已有认证数据不读入，也不借用任何真实服务的令牌。
  await absent(path.join(dataDirectory, 'netease', 'token'));
  await absent(path.join(dataDirectory, 'netease', 'cookie.json'));
  await absent(path.join(dataDirectory, 'netease', 'profiles'));
  await writeNew(path.join(run, 'netease-execution-lock.json'), {
    schemaVersion: 1, pid: process.pid, startedAt: new Date().toISOString(),
    manifestSeal: seal, scriptSha256: sha(await fs.readFile(ownFile)),
  });

  let server, catalogLog, closed = false, closing = false, shutdownSignal = null;
  let pendingEvidence = Promise.resolve();
  const startedAt = Date.now();
  const stop = (name) => {
    shutdownSignal = name;
    closing = true;
    if (server?.listening) {
      server.close();
      server.closeAllConnections();
    }
  };
  process.on('SIGTERM', () => stop('SIGTERM'));
  process.on('SIGINT', () => stop('SIGINT'));
  try {
    const { openCredentials, privateTemporaryDirectory } = await import(pathToFileURL(path.join(project, 'service/credentials.mjs')));
    const credentials = await openCredentials(dataDirectory);
    if (credentials.cookie !== '') denied('UNEXPECTED_NON_ANONYMOUS_CREDENTIALS');
    process.env.TMPDIR = await privateTemporaryDirectory(credentials.directory);
    const { createUpstream } = await import(pathToFileURL(path.join(project, 'service/upstream.mjs')));
    const { createMusicServer } = await import(pathToFileURL(path.join(project, 'service/app.mjs')));
    server = createMusicServer({ upstream: createUpstream(), credentials });
    catalogLog = await fs.open(path.join(run, 'catalog-requests.jsonl'),
      constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
    let sequence = 0;
    const allowed = new Set(['/assistant/artists', '/artist/tracks', '/radio/search', '/radio/playlist', '/search', '/song/detail']);
    const safe = (value) => {
      if (Array.isArray(value)) return value.map(safe);
      if (!value || typeof value !== 'object') return value;
      return Object.fromEntries(Object.entries(value).filter(([key]) =>
        !/token|password|cookie|authorization|secret|key|url/i.test(key)).map(([key, item]) => [key, safe(item)]));
    };
    // 只旁观白名单元数据响应，不记录认证头或音频地址；完整曲目署名留给验收核对 ID。
    server.prependListener('request', (request, response) => {
      const target = new URL(request.url, 'http://127.0.0.1');
      if (!allowed.has(target.pathname)) return;
      const number = ++sequence, startedAt = new Date().toISOString();
      const context = readPrivateJson(path.join(run, 'active-context.json')).catch(() => null);
      const priorEnd = response.end;
      let payload = '', recorded = false;
      response.end = function (chunk, ...args) {
        if (chunk !== undefined && Buffer.byteLength(chunk) <= 512 * 1024) payload = chunk.toString();
        return priorEnd.call(this, chunk, ...args);
      };
      const record = (delivered) => {
        if (recorded) return; recorded = true;
        let metadata = null;
        try { metadata = safe(JSON.parse(payload)); } catch { /* 无完整回执保持未知。 */ }
        const query = Object.fromEntries([...target.searchParams].filter(([key]) =>
          ['keywords', 'id', 'artistId', 'limit', 'offset'].includes(key)));
        pendingEvidence = pendingEvidence.then(async () => {
          await catalogLog.write(encode({ number, path: target.pathname, query, context: await context,
            startedAt, finishedAt: new Date().toISOString(), delivered,
            status: response.statusCode, metadata }) + '\n');
        });
        pendingEvidence.catch(() => {});
      };
      response.once('finish', () => record(true));
      response.once('close', () => record(response.writableFinished));
    });
    const finished = new Promise((resolve, reject) => {
      server.once('close', () => { closed = true; resolve(); });
      server.once('error', () => reject(new Error('ISOLATED_SERVICE_FAILED')));
    });
    // 绑定失败也会拒绝结束Promise；提前挂接处理，避免未处理异常打印堆栈。
    finished.catch(() => {});
    await new Promise((resolve, reject) => {
      server.once('listening', resolve);
      server.once('error', () => reject(new Error('ISOLATED_BIND_FAILED')));
      server.listen(0, '127.0.0.1');
    });
    const port = server.address().port;
    await health(port);
    if (closing) {
      stop(shutdownSignal || 'STOP_BEFORE_READY');
    } else {
      await writeNew(path.join(run, 'netease-connection.json'), {
        schemaVersion: 1, pid: process.pid, port, anonymous: true,
        createdAt: new Date().toISOString(), manifestSeal: seal,
        qtEnvironment: { MUSICPLAYER_NETEASE_PORT: String(port) },
        anonymousScope: '本次启动无账号Cookie；不会读取或复用已有16381服务',
      });
    }
    await finished;
    await pendingEvidence;
    await catalogLog.sync();
    await catalogLog.close();
    catalogLog = null;
    await writeNew(path.join(run, 'netease-stop.json'), {
      schemaVersion: 1, pid: process.pid, stoppedAt: new Date().toISOString(),
      seconds: Math.round((Date.now() - startedAt) / 1000), closed, signal: shutdownSignal,
      evidenceRetained: true,
    });
  } catch {
    stop('STARTUP_FAILED');
    await writeNew(path.join(run, 'netease-error.json'), {
      schemaVersion: 1, pid: process.pid, at: new Date().toISOString(), errorCode: 'ISOLATED_NETEASE_FAILED',
    });
    process.exitCode = 1;
  } finally {
    if (catalogLog) await catalogLog.close().catch(() => {});
  }
}

main().catch(() => {
  // 不打印异常或堆栈，避免依赖库异常对象带出私密内容。
  process.stderr.write('隔离音乐服务启动失败，请检查本次私有运行目录。\n');
  process.exitCode = 1;
});
