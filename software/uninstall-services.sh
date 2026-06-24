#!/usr/bin/env bash
# Remove the Crumb LaunchAgents (serve + sync).
AGENTS="$HOME/Library/LaunchAgents"
for label in com.crumb.serve com.crumb.sync; do
  plist="$AGENTS/$label.plist"
  launchctl unload "$plist" 2>/dev/null || true
  rm -f "$plist"
  echo "removed $label"
done
echo "Done. (The Cloudflare tunnel daemon is separate and left untouched.)"
