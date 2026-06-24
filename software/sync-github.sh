#!/usr/bin/env bash
# Poll GitHub; if the tracked branch moved, pull and restart the server.
# Meant to be run periodically by the com.crumb.sync LaunchAgent.
set -euo pipefail

# LaunchAgents run with a minimal PATH — make sure git resolves.
export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

SOFTWARE_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SOFTWARE_DIR/.." && pwd)"
BRANCH="${CRUMB_SYNC_BRANCH:-main}"
cd "$REPO_DIR"

# GitHub often needs a proxy on China networks. The LaunchAgent does NOT inherit
# your shell proxy, so read it from software/.env (CRUMB_GIT_PROXY=http://127.0.0.1:7890).
if [ -f software/.env ]; then
  GIT_PROXY="$(grep -E '^CRUMB_GIT_PROXY=' software/.env | cut -d= -f2- || true)"
  if [ -n "${GIT_PROXY:-}" ]; then
    export HTTPS_PROXY="$GIT_PROXY" HTTP_PROXY="$GIT_PROXY" ALL_PROXY="$GIT_PROXY"
  fi
fi

if ! git fetch --quiet origin "$BRANCH"; then
  echo "$(date '+%F %T') fetch failed (network?), will retry next interval"
  exit 0
fi

LOCAL="$(git rev-parse HEAD)"
REMOTE="$(git rev-parse "origin/$BRANCH")"

if [ "$LOCAL" = "$REMOTE" ]; then
  exit 0   # already up to date, nothing to do
fi

echo "$(date '+%F %T') update $LOCAL -> $REMOTE, pulling"
CHANGED="$(git diff --name-only "$LOCAL" "$REMOTE")"

if ! git pull --ff-only origin "$BRANCH"; then
  echo "$(date '+%F %T') pull failed (local commits on Mac?). Skipping."
  exit 0
fi

# Reinstall deps only if requirements changed.
if echo "$CHANGED" | grep -q '^software/requirements.txt$'; then
  echo "$(date '+%F %T') requirements.txt changed, reinstalling deps"
  software/.venv/bin/pip install -q -r software/requirements.txt || true
fi

# Restart the server so it runs the new code.
if launchctl kickstart -k "gui/$(id -u)/com.crumb.serve" 2>/dev/null; then
  echo "$(date '+%F %T') restarted com.crumb.serve"
else
  echo "$(date '+%F %T') could not kickstart com.crumb.serve (is it installed?)"
fi
