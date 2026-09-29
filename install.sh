#!/bin/bash
# Build cx158-touch-warp and install it as a per-user launch agent.
#   ./install.sh             quiet logging
#   ./install.sh --verbose   log every touch report and remapped event
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
LABEL=local.cx158-touch-warp
BIN="$DIR/bin/cx158-touch-warp"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/cx158-touch-warp.log"

ARGS="        <string>$BIN</string>"
case "${1:-}" in
    "")        ;;
    --verbose) ARGS="$ARGS
        <string>--verbose</string>" ;;
    *)         echo "usage: $0 [--verbose]" >&2; exit 2 ;;
esac

"$DIR/build.sh"
mkdir -p "$(dirname "$PLIST")" "$(dirname "$LOG")"
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
$ARGS
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>LimitLoadToSessionType</key>
    <string>Aqua</string>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
PLIST
plutil -lint -s "$PLIST"

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "installed $LABEL; log: $LOG"
cat <<MSG

Grant both permissions in System Settings > Privacy & Security:
  Accessibility     applied to the running agent within 10 s
  Input Monitoring  applied to new processes only; the agent relaunches once after 30 s
If cx158-touch-warp is not listed, add $BIN with the + button.
MSG
