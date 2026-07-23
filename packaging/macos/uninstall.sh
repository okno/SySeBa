#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this uninstaller as root: sudo ./uninstall.sh" >&2
    exit 1
fi

PREFIX=${SYSEBA_PREFIX:-/usr/local}
PLIST=/Library/LaunchDaemons/com.okno.syseba.plist
launchctl bootout system/com.okno.syseba >/dev/null 2>&1 || true
rm -f -- "$PLIST" "$PREFIX/bin/syseba"

if [ "${1:-}" = "--purge" ]; then
    rm -rf -- "$PREFIX/etc/syseba" \
        "$PREFIX/var/lib/syseba" \
        "$PREFIX/var/log/syseba" \
        "$PREFIX/var/run/syseba"
fi
echo "SySeBa removed. Configuration and state were preserved unless --purge was used."
