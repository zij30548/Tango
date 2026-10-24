#!/bin/bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/development-env.sh"
/bin/bash "$script_dir/build-macos.sh"
/bin/bash "$script_dir/ensure-app-backend.sh"
