// 某些上游模块在请求失败时会记录原始响应对象
// （含账号 Cookie）；导入前先禁用其控制台输出。
for (const method of ['log', 'info', 'warn', 'error', 'debug', 'trace', 'dir']) {
  console[method] = () => {};
}

try {
  const { openCredentials, privateTemporaryDirectory } = await import('./credentials.mjs');
  const credentials = await openCredentials();
  process.env.TMPDIR = await privateTemporaryDirectory(credentials.directory);
  const { createUpstream } = await import('./upstream.mjs');
  const { createMusicServer } = await import('./app.mjs');
  const server = createMusicServer({ upstream: createUpstream(), credentials });
  server.on('error', () => {
    process.stderr.write('本机音乐服务启动失败：端口被占用或配置不可用。\n');
    process.exitCode = 1;
  });
  server.listen(16381, '127.0.0.1');
  const stop = () => {
    server.close();
    server.closeAllConnections();
  };
  process.on('SIGINT', stop);
  process.on('SIGTERM', stop);
} catch {
  process.stderr.write('本机音乐服务启动失败。请运行 scripts/setup-music-service.sh 检查依赖和数据目录。\n');
  process.exitCode = 1;
}
