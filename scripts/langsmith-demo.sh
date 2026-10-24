#!/bin/bash
# 隔离业务链路演示：默认只保存本机摘要，显式 --upload 才读取 LangSmith 配置。
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python_bin="$project_dir/backend_py/.venv/bin/python"
if [[ ! -x "$python_bin" ]]; then
  printf '%s\n' '缺少项目 Python 环境，请先按 backend_py/README.md 准备。' >&2
  exit 1
fi
cd "$project_dir/backend_py"
exec "$python_bin" -m evaluation.langsmith_demo "$@"
