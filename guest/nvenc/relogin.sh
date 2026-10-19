#!/bin/bash
# Log the guest user out and let auto-login bring the session back (fresh WindowServer + apps load the current plugin).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/../../scripts/config.sh"
"$REPO/vm/ssh-askpass-env.sh" | ssh "$GUEST_SSH" 'sudo -S -p "" sh -c "launchctl bootout gui/$(id -u) 2>/dev/null; sleep 6; killall loginwindow 2>/dev/null; true"'
for i in $(seq 1 60); do sleep 3; u=$(ssh "$GUEST_SSH" 'stat -f%Su /dev/console; pgrep -x Finder >/dev/null && echo finder' 2>/dev/null | tr '\n' ' ' || true); case "$u" in "$GUEST_USER finder ") echo "logged in again after $((i*3)) s"; exit 0;; esac; done
echo "no auto-login after 180 s: $u"; exit 1
