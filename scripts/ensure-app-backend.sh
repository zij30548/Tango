#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
app_backend_url="${MUSICPLAYER_BACKEND_URL:-http://127.0.0.1:16382}"
case "$app_backend_url" in
  http://127.0.0.1:*|http://localhost:*) ;;
  *) printf '%s\n' '使用配置的探歌远程后台。'; exit 0 ;;
esac
/bin/bash "$project_dir/scripts/ensure-python-backend.sh"
if [[ ! -f "$project_dir/backend_py/.env" ]]; then
  (umask 077; cp "$project_dir/backend_py/.env.example" "$project_dir/backend_py/.env")
fi
chmod 600 "$project_dir/backend_py/.env"
# 当前后台与基础设施均由 Python 启动，旧数据库只能显式迁移。
"$project_dir/backend_py/.venv/bin/python" -m tango_backend.ensure_storage
exec "$project_dir/backend_py/.venv/bin/python" -m tango_backend.launch_local "$app_backend_url"
