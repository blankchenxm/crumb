#!/usr/bin/env bash
# Foreground server launcher for the LaunchAgent.
# launchd manages the process lifecycle, so this must run in the FOREGROUND
# (no nohup, no &). Use ./run.sh instead for a manual background start.
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d .venv ]; then
  echo "ERROR: .venv missing. Run ./setup.sh first." >&2
  exit 1
fi
if [ ! -f .env ]; then
  echo "ERROR: .env missing. Run ./setup.sh, then fill in software/.env." >&2
  exit 1
fi

# Load API keys.
set -a
source .env
set +a

# Server only calls api.siliconflow.cn (in China) — must NOT go through a proxy.
unset HTTP_PROXY HTTPS_PROXY ALL_PROXY http_proxy https_proxy all_proxy

# exec so launchd tracks the uvicorn process directly.
exec .venv/bin/uvicorn server:app --host 127.0.0.1 --port 8000
