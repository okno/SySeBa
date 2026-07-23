#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root: sudo ./install.sh" >&2
    exit 1
fi

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
PREFIX=${SYSEBA_PREFIX:-/usr/local}
CONFIG_DIR="$PREFIX/etc/syseba"
STATE_DIR="$PREFIX/var/lib/syseba"
LOG_DIR="$PREFIX/var/log/syseba"
RUN_DIR="$PREFIX/var/run/syseba"
PLIST=/Library/LaunchDaemons/com.okno.syseba.plist

install -d -m 0755 "$PREFIX/bin" "$PREFIX/share/syseba"
install -d -m 0750 "$CONFIG_DIR" "$STATE_DIR" "$LOG_DIR" "$RUN_DIR"
install -m 0755 "$ROOT/bin/syseba" "$PREFIX/bin/syseba"
if [ ! -f "$CONFIG_DIR/syseba.conf" ]; then
    install -m 0640 "$ROOT/etc/syseba/syseba.conf" "$CONFIG_DIR/syseba.conf"
fi
install -m 0644 \
    "$ROOT/share/syseba/com.okno.syseba.plist" \
    "$PLIST"
install -d -m 0750 /Users/Shared/SySeBa/source \
    /Users/Shared/SySeBa/backup \
    /Users/Shared/SySeBa/restore

launchctl bootout system/com.okno.syseba >/dev/null 2>&1 || true
launchctl bootstrap system "$PLIST"
launchctl enable system/com.okno.syseba
launchctl kickstart -k system/com.okno.syseba

echo "SySeBa installed and started."
echo "Web UI: http://<mac-ip>:8765"
echo "Token:  $CONFIG_DIR/syseba_web.token"
