#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$project_dir/scripts/development-env.sh"
/bin/bash "$project_dir/scripts/prepare-macos.sh"
exec "$project_dir/build-macos/Tango.app/Contents/MacOS/Tango" "$@"
