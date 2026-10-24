#!/bin/bash
# Python 依赖只安装到项目虚拟环境，不改变系统 Python。
set -euo pipefail
tango_python_project="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if ! command -v uv >/dev/null 2>&1; then
  printf '%s\n' '请先安装 uv（macOS: brew install uv），用于准备 Python 3.12 后台。' >&2
  exit 1
fi
uv sync --project "$tango_python_project/backend_py" --python 3.12 --frozen --inexact --no-dev
unset tango_python_project
