#!/usr/bin/env bash
set -Eeuo pipefail

ROOT=$(mktemp -d "${TMPDIR:-/tmp}/syseba-maintenance-test.XXXXXX")
INSTALL="$ROOT/opt/syseba"
BACKUPS="$ROOT/backups"
MOCK_BIN="$ROOT/bin"
UNIT="$ROOT/etc/syseba.service"
STATE="$ROOT/systemd.state"

cleanup() {
    rm -rf -- "$ROOT"
}
trap cleanup EXIT INT TERM

mkdir -p "$INSTALL/source" "$INSTALL/backup" "$INSTALL/restore" \
    "$MOCK_BIN" "$(dirname "$UNIT")"
printf 'legacy-version\n' >"$INSTALL/syseba.py"
printf 'database-v1\n' >"$INSTALL/syseba_logs.db"
printf 'token-v1\n' >"$INSTALL/syseba_web.token"
cat >"$INSTALL/syseba.conf" <<EOF
[SETTINGS]
source = $INSTALL/source
backup = $INSTALL/backup
restore = $INSTALL/restore
log = $INSTALL/syseba.log
threads = 2
EOF
printf '[Unit]\nDescription=mock\n' >"$UNIT"
printf 'active\n' >"$STATE"

cat >"$MOCK_BIN/systemctl" <<'EOF'
#!/usr/bin/env bash
set -eu
case "$1" in
    is-active)
        grep -qx active "$MOCK_SYSTEMD_STATE"
        ;;
    is-enabled)
        exit 0
        ;;
    show)
        printf '%s\n' "$MOCK_SYSTEMD_UNIT"
        ;;
    stop)
        printf 'inactive\n' >"$MOCK_SYSTEMD_STATE"
        ;;
    start|restart)
        printf 'active\n' >"$MOCK_SYSTEMD_STATE"
        ;;
    enable|daemon-reload|status|disable)
        exit 0
        ;;
    *)
        printf 'unexpected systemctl command: %s\n' "$*" >&2
        exit 1
        ;;
esac
EOF
chmod 0755 "$MOCK_BIN/systemctl"

export PATH="$MOCK_BIN:$PATH"
export MOCK_SYSTEMD_STATE="$STATE"
export MOCK_SYSTEMD_UNIT="$UNIT"
export SYSEBA_ALLOW_NON_ROOT=1
export SYSEBA_INSTALL_DIR="$INSTALL"
export SYSEBA_BACKUP_ROOT="$BACKUPS"
export SYSEBA_UNIT_FILE="$UNIT"
export SYSEBA_CONFIG_PATH="$INSTALL/syseba.conf"
export SYSEBA_DB_PATH="$INSTALL/syseba_logs.db"
export SYSEBA_TOKEN_PATH="$INSTALL/syseba_web.token"
export SYSEBA_LOCK_PATH="$INSTALL/syseba.lock"
export SYSEBA_HEALTH_TIMEOUT=5

MAINTENANCE=${1:?maintenance script path required}
"$MAINTENANCE" backup >/dev/null
test -f "$BACKUPS/LATEST"
SNAPSHOT=$(tr -d '\r\n' <"$BACKUPS/LATEST")
test -f "$BACKUPS/$SNAPSHOT/SHA256SUMS"

printf 'changed-version\n' >"$INSTALL/syseba.py"
printf 'database-v2\n' >"$INSTALL/syseba_logs.db"
"$MAINTENANCE" rollback "$SNAPSHOT" >/dev/null

grep -qx legacy-version "$INSTALL/syseba.py"
grep -qx database-v1 "$INSTALL/syseba_logs.db"
grep -qx active "$STATE"
(cd "$BACKUPS/$SNAPSHOT" && sha256sum -c SHA256SUMS >/dev/null)
printf 'maintenance integration: OK\n'
