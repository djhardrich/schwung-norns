#!/bin/sh
# restart-norns.sh — Restart Norns stack in the chroot
# Use this instead of ";restart" in Maiden (which requires systemd/dbus).
#
# Usage:  restart-norns.sh [slot]
#   slot defaults to 1
#
# Can be called via pw-helper:  pw-helper restart <slot>
# Or directly via SSH:          sh /data/UserData/schwung/modules/tools/norns/restart-norns.sh

SLOT="${1:-1}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FIFO_PLAYBACK="/tmp/pw-to-move-${SLOT}"

if [ ! -e "$FIFO_PLAYBACK" ]; then
    echo "ERROR: FIFO $FIFO_PLAYBACK not found — is the module loaded?" >&2
    exit 1
fi

echo "Restarting Norns (slot $SLOT)..."

sh "$SCRIPT_DIR/stop-norns.sh" "$SLOT"
sleep 2
sh "$SCRIPT_DIR/start-norns.sh" "$FIFO_PLAYBACK" "$SLOT"
