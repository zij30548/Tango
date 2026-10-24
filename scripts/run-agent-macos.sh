#!/bin/bash
set -euo pipefail
# 保留旧启动入口兼容；普通用户无需配置模型服务密钥。
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec /bin/bash "$project_dir/scripts/run-macos.sh" "$@"
