#!/usr/bin/env bash
set -euo pipefail
export PATH="${MUSICPLAYER_NODE_PREFIX:-/opt/homebrew/opt/node@24}/bin:/opt/homebrew/opt/node@22/bin:/opt/homebrew/bin:/usr/local/opt/node@24/bin:/usr/local/opt/node@22/bin:/usr/local/bin:$PATH"
project_directory="$(cd "$(dirname "$0")/.." && pwd)"
if ! command -v node >/dev/null 2>&1; then
  echo '未找到 Node.js，请安装 Node.js 20 或更新版本，并运行 bash scripts/setup-music-service.sh。' >&2
  exit 1
fi
exec node "$project_directory/service/launch.mjs"
