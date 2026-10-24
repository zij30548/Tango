import http from 'node:http';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const require = createRequire(import.meta.url);

export function serviceHealthStatus(result, status) {
  // 已知旧进程不能充当支持按艺人 ID 检索的新服务；只提示正常关闭，不终止未知进程。
  if (status !== 200 || !result || result.app !== 'quiet-netease' || result.version !== 1 ||
      !Number.isSafeInteger(result.pid) || result.pid <= 1) return 'occupied';
  return result.profileIsolation === 1 && result.catalogVersion === 2 ? 'ready' : 'outdated';
}

async function health() {
  return new Promise((resolve) => {
    const request = http.get('http://127.0.0.1:16381/health', { timeout: 500 }, (response) => {
      let text = '';
      response.setEncoding('utf8');
      response.on('data', (part) => {
        text += part;
        if (text.length > 2048) request.destroy();
      });
      response.on('end', () => {
        try {
          const result = JSON.parse(text);
          resolve(serviceHealthStatus(result, response.statusCode));
        } catch { resolve('occupied'); }
      });
    });
    request.on('timeout', () => request.destroy());
    request.on('error', (error) => resolve(error.code === 'ECONNREFUSED' ? 'absent' : 'occupied'));
  });
}

export async function main() {
 try {
  if (Number(process.versions.node.split('.')[0]) < 20) throw new Error('node_version');
  const initial = await health();
  if (initial === 'ready') return 0;
  if (initial === 'outdated') {
    process.stderr.write('本机音乐服务版本过旧，请正常关闭旧版探歌及其音乐服务后重新启动；不会自动终止已有进程。\n');
    return 1;
  }
  if (initial === 'occupied') {
    process.stderr.write('端口 16381 已被其他程序占用，无法启动本机音乐服务。\n');
    return 1;
  }
  if (require('NeteaseCloudMusicApi/package.json').version !== '4.32.0') throw new Error('dependency');
  const child = spawn(process.execPath, [fileURLToPath(new URL('./server.mjs', import.meta.url))], {
    detached: true,
    stdio: 'ignore',
    env: process.env,
    windowsHide: true,
  });
  child.on('error', () => {});
  child.unref();
  for (let attempt = 0; attempt < 40; ++attempt) {
    await new Promise((resolve) => setTimeout(resolve, 100));
    if (await health() === 'ready') return 0;
  }
  process.stderr.write('本机音乐服务启动失败，请检查数据目录权限或运行 scripts/setup-music-service.sh。\n');
  return 1;
 } catch {
  process.stderr.write('请先安装 Node.js 20 或更新版本，然后运行 bash scripts/setup-music-service.sh。\n');
  return 1;
 }
}

// 导入纯健康判定供离线测试时不探测端口、不启动服务。
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url))
  process.exitCode = await main();
