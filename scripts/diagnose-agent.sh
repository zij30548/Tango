#!/bin/bash
# 本机维护者只读入口：固定选择当前项目 MySQL 配置，不启动服务或加载模型配置。
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python_bin="$project_dir/backend_py/.venv/bin/python"
if [[ ! -x "$python_bin" ]]; then
  printf '%s\n' '缺少项目 Python 环境，请先按 README 准备开发环境。' >&2
  exit 1
fi
for argument in "$@"; do
  case "$argument" in
    --env-file|--env-file=*|--sqlite|--sqlite=*)
      printf '%s\n' '该快捷入口固定查看本机 MySQL；其他库请直接使用 tango_backend.diagnostics。' >&2
      exit 2 ;;
  esac
done
cd "$project_dir"
exec "$python_bin" -m tango_backend.diagnostics "$@" --env-file "$project_dir/.local/mysql-v1/storage.env"
