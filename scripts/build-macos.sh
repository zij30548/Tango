#!/bin/bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
qt_prefix="${MUSICPLAYER_QT_PREFIX:-/opt/homebrew/opt/qt@5}"
hiredis_prefix="${MUSICPLAYER_HIREDIS_PREFIX:-/opt/homebrew/opt/hiredis}"
app_backend_url="${MUSICPLAYER_BACKEND_URL:-http://127.0.0.1:16382}"
if [[ ! "$app_backend_url" =~ ^https?://[a-zA-Z0-9][a-zA-Z0-9.:-]*(/[a-zA-Z0-9_-]+)*/?$ ]]; then
  printf '%s\n' '探歌后台地址格式无效。请使用不含密钥、查询参数的服务基础地址。' >&2
  exit 1
fi
if [[ ! -x "$qt_prefix/bin/qmake" && -x /usr/local/opt/qt@5/bin/qmake ]]; then
  qt_prefix=/usr/local/opt/qt@5
fi
if [[ ! -d "$hiredis_prefix/include/hiredis" && -d /usr/local/opt/hiredis/include/hiredis ]]; then
  hiredis_prefix=/usr/local/opt/hiredis
fi
if [[ ! -x "$qt_prefix/bin/qmake" || ! -d "$hiredis_prefix/include/hiredis" ]]; then
  printf '%s\n' '缺少 Qt 5 或 hiredis。请先执行 brew install qt@5 hiredis redis。' >&2
  exit 1
fi
mkdir -p "$project_dir/build-macos"
cd "$project_dir/build-macos"
"$qt_prefix/bin/qmake" "$project_dir/MusicPlayer2.pro" "HIREDIS_PREFIX=$hiredis_prefix" "APP_BACKEND_URL=$app_backend_url" CONFIG+=debug CONFIG-=release
/usr/bin/make -j "${MUSICPLAYER_BUILD_JOBS:-4}"
printf '%s\n' "编译完成：$project_dir/build-macos/Tango.app"
