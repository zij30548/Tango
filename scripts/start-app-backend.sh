#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
/bin/bash "$project_dir/scripts/ensure-python-backend.sh"
if [[ ! -f "$project_dir/backend_py/.env" ]]; then
  (umask 077; cp "$project_dir/backend_py/.env.example" "$project_dir/backend_py/.env")
fi
chmod 600 "$project_dir/backend_py/.env"
"$project_dir/backend_py/.venv/bin/python" -m tango_backend.ensure_storage
exec "$project_dir/backend_py/.venv/bin/python" -m tango_backend.server "$@"
