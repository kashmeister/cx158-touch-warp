#!/bin/bash
# Stop and remove the launch agent. The source tree and log are left in place.
set -euo pipefail
LABEL=local.cx158-touch-warp
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null && echo "stopped $LABEL" || true
rm -f "$PLIST"
echo "removed $PLIST"
echo "Remove the cx158-touch-warp entries under System Settings > Privacy & Security >"
echo "Accessibility and Input Monitoring by hand; macOS offers no command for unbundled binaries."
