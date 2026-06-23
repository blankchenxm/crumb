#!/usr/bin/env bash
# Start the ASR server in the background (nohup) on 127.0.0.1:8000,
# which is the port the Cloudflare tunnel forwards to.
# Usage:  ./run.sh
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d .venv ]; then
  echo "ERROR: .venv not found. Run ./setup.sh first." >&2
  exit 1
fi

if [ ! -f .env ]; then
  echo "ERROR: .env not found. Run ./setup.sh, then fill in software/.env." >&2
  exit 1
fi

# Load API keys from .env into the environment.
set -a
source .env
set +a

# This server only calls api.siliconflow.cn (in China) — it must NOT go through
# a proxy. Drop any proxy vars inherited from the shell so requests go direct.
unset HTTP_PROXY HTTPS_PROXY ALL_PROXY http_proxy https_proxy all_proxy

source .venv/bin/activate

mkdir -p logs
nohup uvicorn server:app --host 127.0.0.1 --port 8000 > logs/server.log 2>&1 &
echo $! > server.pid

echo "Server started in background (PID $(cat server.pid))."
echo "  Logs:  tail -f software/logs/server.log"
echo "  Stop:  ./stop.sh"
