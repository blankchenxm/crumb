#!/usr/bin/env bash
# Install macOS LaunchAgents for the Crumb server:
#   com.crumb.serve : runs the server, restarts on crash, starts at login
#   com.crumb.sync  : every N seconds, pull from GitHub & restart server on changes
# Run once on the Mac:  cd software && ./install-services.sh
set -euo pipefail
cd "$(dirname "$0")"
SOFTWARE_DIR="$(pwd)"

AGENTS="$HOME/Library/LaunchAgents"
SERVE_PLIST="$AGENTS/com.crumb.serve.plist"
SYNC_PLIST="$AGENTS/com.crumb.sync.plist"
INTERVAL="${CRUMB_SYNC_INTERVAL:-60}"

mkdir -p "$AGENTS" logs
chmod +x serve.sh sync-github.sh

# Ensure venv + .env exist before daemonizing.
if [ ! -d .venv ] || [ ! -f .env ]; then
  echo "==> .venv or .env missing, running setup.sh first"
  ./setup.sh
fi

# Capture the current shell proxy so the sync agent can reach GitHub
# (LaunchAgents don't read ~/.zshrc). Only if not already recorded.
if [ -n "${HTTPS_PROXY:-}" ] && ! grep -q '^CRUMB_GIT_PROXY=' .env 2>/dev/null; then
  echo "CRUMB_GIT_PROXY=$HTTPS_PROXY" >> .env
  echo "==> Captured proxy for GitHub sync: $HTTPS_PROXY"
fi

# Stop any manual (nohup) server to avoid a port clash.
./stop.sh 2>/dev/null || true

cat > "$SERVE_PLIST" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.crumb.serve</string>
    <key>ProgramArguments</key>
    <array>
        <string>$SOFTWARE_DIR/serve.sh</string>
    </array>
    <key>WorkingDirectory</key>
    <string>$SOFTWARE_DIR</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$SOFTWARE_DIR/logs/serve.log</string>
    <key>StandardErrorPath</key>
    <string>$SOFTWARE_DIR/logs/serve.log</string>
    <key>ThrottleInterval</key>
    <integer>5</integer>
</dict>
</plist>
EOF

cat > "$SYNC_PLIST" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.crumb.sync</string>
    <key>ProgramArguments</key>
    <array>
        <string>/bin/bash</string>
        <string>$SOFTWARE_DIR/sync-github.sh</string>
    </array>
    <key>StartInterval</key>
    <integer>$INTERVAL</integer>
    <key>RunAtLoad</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$SOFTWARE_DIR/logs/sync.log</string>
    <key>StandardErrorPath</key>
    <string>$SOFTWARE_DIR/logs/sync.log</string>
</dict>
</plist>
EOF

launchctl unload "$SERVE_PLIST" 2>/dev/null || true
launchctl unload "$SYNC_PLIST" 2>/dev/null || true
launchctl load "$SERVE_PLIST"
launchctl load "$SYNC_PLIST"

echo
echo "Installed LaunchAgents:"
echo "  com.crumb.serve  -> always-on server   (logs/serve.log)"
echo "  com.crumb.sync   -> git pull every ${INTERVAL}s (logs/sync.log)"
echo
echo "Local server: http://127.0.0.1:8000"
echo "Uninstall:    ./uninstall-services.sh"
