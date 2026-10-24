#!/bin/bash
# 由本机运行或准备脚本加载，不读取模型配置。
tango_dev_project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${MUSICPLAYER_BACKEND_URL:-}" ]]; then
  if [[ "${MUSICPLAYER_DEVELOPMENT_MODE:-1}" == "1" ]]; then
    export MUSICPLAYER_BACKEND_URL="http://127.0.0.1:16383"
  else
    export MUSICPLAYER_BACKEND_URL="http://127.0.0.1:16382"
  fi
fi
case "$MUSICPLAYER_BACKEND_URL" in
  http://127.0.0.1:*) export MUSICPLAYER_DEVELOPMENT_MODE="${MUSICPLAYER_DEVELOPMENT_MODE:-1}" ;;
  *) export MUSICPLAYER_DEVELOPMENT_MODE="${MUSICPLAYER_DEVELOPMENT_MODE:-0}" ;;
esac
if [[ "$MUSICPLAYER_DEVELOPMENT_MODE" == "1" ]]; then
  export MUSICPLAYER_DEVELOPMENT_TOKEN_FILE="${MUSICPLAYER_DEVELOPMENT_TOKEN_FILE:-$tango_dev_project_dir/.local/development/session.token}"
fi
unset tango_dev_project_dir
