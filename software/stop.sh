#!/usr/bin/env bash
# Stop the background server started by run.sh.
cd "$(dirname "$0")"

if [ -f server.pid ]; then
  PID="$(cat server.pid)"
  if kill "$PID" 2>/dev/null; then
    echo "Stopped server (PID $PID)."
  else
    echo "Process $PID not running."
  fi
  rm -f server.pid
else
  echo "No server.pid found — is the server running?"
fi
