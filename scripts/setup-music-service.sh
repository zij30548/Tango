#!/usr/bin/env bash
set -euo pipefail
export PATH="${MUSICPLAYER_NODE_PREFIX:-/opt/homebrew/opt/node@24}/bin:/opt/homebrew/opt/node@22/bin:/opt/homebrew/bin:/usr/local/opt/node@24/bin:/usr/local/opt/node@22/bin:/usr/local/bin:$PATH"
project_directory="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
  echo '请先安装 Node.js 20 或更新版本（包含 npm）。' >&2
  exit 1
fi
if ! node -e 'process.exit(Number(process.versions.node.split(".")[0]) >= 20 ? 0 : 1)'; then
  echo '本机音乐服务需要 Node.js 20 或更新版本。' >&2
  exit 1
fi
cd "$project_directory/service"
npm ci --omit=dev --ignore-scripts --no-audit --no-fund
echo '网易云本机服务依赖已准备好。使用搜索或网易云扫码登录时会自动启动。'
