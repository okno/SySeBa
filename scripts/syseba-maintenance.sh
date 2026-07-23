#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'
umask 077

SCRIPT_NAME="$(basename "$0")"
SCRIPT_VERSION="2.0"
SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
SOURCE_ROOT="$(dirname "$SCRIPT_DIR")"
LAUNCH_DIR="$(pwd -P)"

INSTALL_DIR="${SYSEBA_INSTALL_DIR:-/opt/syseba}"
BACKUP_ROOT="${SYSEBA_BACKUP_ROOT:-${LAUNCH_DIR}/syseba-backups}"
SERVICE_NAME="${SYSEBA_SERVICE:-syseba.service}"
UNIT_OVERRIDE="${SYSEBA_UNIT_FILE:-/etc/systemd/system/${SERVICE_NAME}}"
CONFIG_PATH="${SYSEBA_CONFIG_PATH:-${INSTALL_DIR}/syseba.conf}"
DB_PATH="${SYSEBA_DB_PATH:-${INSTALL_DIR}/syseba_logs.db}"
TOKEN_PATH="${SYSEBA_TOKEN_PATH:-${INSTALL_DIR}/syseba_web.token}"
LOCK_PATH="${SYSEBA_LOCK_PATH:-${INSTALL_DIR}/syseba.lock}"
WEB_HOST="${SYSEBA_WEB_HOST:-0.0.0.0}"
WEB_PORT="${SYSEBA_WEB_PORT:-8765}"
REPO_URL="${SYSEBA_REPO_URL:-https://github.com/okno/SySeBa.git}"
DEFAULT_REF="${SYSEBA_REF:-main}"
HEALTH_TIMEOUT="${SYSEBA_HEALTH_TIMEOUT:-30}"
MAINTENANCE_LOCK="${SYSEBA_MAINTENANCE_LOCK:-${BACKUP_ROOT}/.maintenance.lock}"

TEMP_ROOT=""
FOLLOW_PID=""
declare -a EXTERNAL_PATHS=()

log() {
    printf '[SySeBa] %s\n' "$*" >&2
}

warn() {
    printf '[SySeBa] WARNING: %s\n' "$*" >&2
}

die() {
    printf '[SySeBa] ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<EOF
SySeBa native maintenance tool ${SCRIPT_VERSION}

Usage:
  ${SCRIPT_NAME} install-local
  ${SCRIPT_NAME} quick-update [branch|tag|commit]
  ${SCRIPT_NAME} update [branch|tag|commit]
  ${SCRIPT_NAME} backup
  ${SCRIPT_NAME} rollback [snapshot-id|latest|pre-update]
  ${SCRIPT_NAME} list
  ${SCRIPT_NAME} verify
  ${SCRIPT_NAME} logs [lines]
  ${SCRIPT_NAME} follow

Commands:
  install-local  Build this source tree, back up the installed version and
                 migrate it to the native C executable.
  quick-update   Compare local and remote revisions; update only when needed.
  update         Clone the selected Git revision, build, test and install it.
  backup         Stop the service, create a consistent snapshot, then restart.
  rollback       Select a snapshot in text mode or restore the requested ID.
  list           List snapshots and their exact application identity.
  verify         Validate executable, configuration, service and Web listener.
  logs           Show systemd and application logs.
  follow         Follow both log streams until Ctrl+C.

Defaults:
  install:       ${INSTALL_DIR}
  snapshots:     ${BACKUP_ROOT}
  configuration: ${CONFIG_PATH}
  database:      ${DB_PATH}
  service:       ${SERVICE_NAME}
  Web UI:        ${WEB_HOST}:${WEB_PORT}

Environment overrides:
  SYSEBA_INSTALL_DIR, SYSEBA_BACKUP_ROOT, SYSEBA_SERVICE,
  SYSEBA_UNIT_FILE, SYSEBA_CONFIG_PATH, SYSEBA_DB_PATH,
  SYSEBA_TOKEN_PATH, SYSEBA_LOCK_PATH, SYSEBA_WEB_HOST,
  SYSEBA_WEB_PORT, SYSEBA_REPO_URL, SYSEBA_REF,
  SYSEBA_HEALTH_TIMEOUT, SYSEBA_MAINTENANCE_LOCK

Configured source, backup and restore data trees are never archived, moved,
deleted or replaced by this tool.
EOF
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

require_command() {
    command_exists "$1" || die "Required command not found: $1"
}

normalize_path() {
    readlink -m -- "$1"
}

path_is_within() {
    local base candidate
    base="$(normalize_path "$1")"
    candidate="$(normalize_path "$2")"
    [[ "$candidate" == "$base" || "$candidate" == "$base"/* ]]
}

validate_path_setting() {
    local name="$1"
    local value="$2"
    [[ "$value" == /* && "$value" != "/" ]] ||
        die "Unsafe ${name}: ${value}"
    [[ "$value" != *$'\n'* && "$value" != *$'\r'* ]] ||
        die "${name} contains a line break"
}

validate_settings() {
    require_command readlink
    INSTALL_DIR="$(normalize_path "$INSTALL_DIR")"
    BACKUP_ROOT="$(normalize_path "$BACKUP_ROOT")"
    UNIT_OVERRIDE="$(normalize_path "$UNIT_OVERRIDE")"
    CONFIG_PATH="$(normalize_path "$CONFIG_PATH")"
    DB_PATH="$(normalize_path "$DB_PATH")"
    TOKEN_PATH="$(normalize_path "$TOKEN_PATH")"
    LOCK_PATH="$(normalize_path "$LOCK_PATH")"
    MAINTENANCE_LOCK="$(normalize_path "$MAINTENANCE_LOCK")"

    validate_path_setting install_dir "$INSTALL_DIR"
    validate_path_setting backup_root "$BACKUP_ROOT"
    validate_path_setting unit_file "$UNIT_OVERRIDE"
    validate_path_setting config_path "$CONFIG_PATH"
    validate_path_setting db_path "$DB_PATH"
    validate_path_setting token_path "$TOKEN_PATH"
    validate_path_setting lock_path "$LOCK_PATH"
    validate_path_setting maintenance_lock "$MAINTENANCE_LOCK"
    path_is_within "$INSTALL_DIR" "$BACKUP_ROOT" &&
        die "Snapshot root cannot be inside the install directory"
    [[ "$SERVICE_NAME" =~ ^[A-Za-z0-9_.@-]+$ ]] ||
        die "Invalid systemd service name: $SERVICE_NAME"
    [[ "$WEB_HOST" =~ ^[A-Za-z0-9._:-]+$ ]] ||
        die "Invalid Web listen address: $WEB_HOST"
    if [[ ! "$WEB_PORT" =~ ^[1-9][0-9]{0,4}$ ]] ||
        ((10#$WEB_PORT > 65535)); then
        die "SYSEBA_WEB_PORT must be between 1 and 65535"
    fi
    [[ "$HEALTH_TIMEOUT" =~ ^[1-9][0-9]{0,3}$ ]] ||
        die "SYSEBA_HEALTH_TIMEOUT must be between 1 and 9999"
}

require_root() {
    if [[ "$EUID" -ne 0 && "${SYSEBA_ALLOW_NON_ROOT:-0}" != "1" ]]; then
        die "Run this command as root (sudo ${SCRIPT_NAME} ...)"
    fi
}

acquire_lock() {
    require_command flock
    mkdir -p -- "$(dirname "$MAINTENANCE_LOCK")"
    chmod 700 -- "$(dirname "$MAINTENANCE_LOCK")" 2>/dev/null || true
    exec 9>"$MAINTENANCE_LOCK"
    flock -n 9 || die "Another SySeBa maintenance operation is running"
}

cleanup() {
    local status=$?
    if [[ -n "$FOLLOW_PID" ]] && kill -0 "$FOLLOW_PID" 2>/dev/null; then
        kill "$FOLLOW_PID" 2>/dev/null || true
        wait "$FOLLOW_PID" 2>/dev/null || true
    fi
    if [[ -n "$TEMP_ROOT" && -d "$TEMP_ROOT" ]]; then
        local parent name
        parent="$(dirname "$TEMP_ROOT")"
        name="$(basename "$TEMP_ROOT")"
        if [[ "$parent" == "$(dirname "$INSTALL_DIR")" &&
              "$name" == .syseba-maintenance.* ]]; then
            rm -rf -- "$TEMP_ROOT"
        else
            warn "Refusing to remove unexpected temporary path: $TEMP_ROOT"
        fi
    fi
    return "$status"
}
trap cleanup EXIT

service_active() {
    systemctl is-active --quiet "$SERVICE_NAME" >/dev/null 2>&1
}

service_enabled() {
    systemctl is-enabled --quiet "$SERVICE_NAME" >/dev/null 2>&1
}

service_fragment() {
    local fragment
    fragment="$(systemctl show "$SERVICE_NAME" \
        --property=FragmentPath --value 2>/dev/null || true)"
    if [[ "$fragment" == /* && -f "$fragment" ]]; then
        printf '%s\n' "$fragment"
    fi
}

stop_service() {
    if ! service_active; then
        return 0
    fi
    log "Stopping ${SERVICE_NAME}..."
    systemctl stop "$SERVICE_NAME"
    local attempt
    for ((attempt = 0; attempt < 30; attempt++)); do
        service_active || return 0
        sleep 1
    done
    die "${SERVICE_NAME} did not stop within 30 seconds"
}

start_service() {
    log "Starting ${SERVICE_NAME}..."
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME" >/dev/null
    systemctl restart "$SERVICE_NAME"
}

read_config_value() {
    local key="$1"
    [[ -f "$CONFIG_PATH" ]] || return 0
    awk -F= -v wanted="$key" '
        function trim(value) {
            sub(/^[[:space:]]+/, "", value)
            sub(/[[:space:]]+$/, "", value)
            return value
        }
        /^[[:space:]]*\[/ {
            section = tolower(trim($0))
            next
        }
        section == "[settings]" {
            name = tolower(trim($1))
            if (name == wanted) {
                sub(/^[^=]*=/, "")
                print trim($0)
                exit
            }
        }
    ' "$CONFIG_PATH"
}

read_log_path() {
    local value
    value="$(read_config_value log)"
    [[ -n "$value" ]] || return 0
    if [[ "$value" != /* ]]; then
        value="$(dirname "$CONFIG_PATH")/$value"
    fi
    normalize_path "$value"
}

installed_identity() {
    local commit digest version
    if [[ -x "$INSTALL_DIR/syseba" ]]; then
        version="$("$INSTALL_DIR/syseba" --version 2>/dev/null || true)"
        digest="$(sha256sum "$INSTALL_DIR/syseba" | awk '{print $1}')"
        commit="$(awk -F= '$1 == "source_commit" {print $2; exit}' \
            "$INSTALL_DIR/BUILD-INFO" 2>/dev/null || true)"
        printf 'native:%s:%s:%s\n' \
            "${version:-unknown}" "${commit:-local}" "$digest"
        return 0
    fi
    if [[ -d "$INSTALL_DIR/.git" ]]; then
        commit="$(git -C "$INSTALL_DIR" rev-parse HEAD 2>/dev/null || true)"
        [[ -z "$commit" ]] || {
            printf 'git:%s\n' "$commit"
            return 0
        }
    fi
    if [[ -f "$INSTALL_DIR/syseba.py" ]]; then
        digest="$(sha256sum "$INSTALL_DIR/syseba.py" | awk '{print $1}')"
        printf 'python:sha256:%s\n' "$digest"
        return 0
    fi
    printf 'unknown\n'
}

add_external_path() {
    local path="$1"
    [[ -e "$path" || -L "$path" ]] || return 0
    path_is_within "$INSTALL_DIR" "$path" && return 0
    local existing
    for existing in "${EXTERNAL_PATHS[@]}"; do
        [[ "$existing" == "$path" ]] && return 0
    done
    EXTERNAL_PATHS+=("$path")
}

collect_external_state() {
    local log_path
    EXTERNAL_PATHS=()
    add_external_path "$CONFIG_PATH"
    add_external_path "$DB_PATH"
    add_external_path "${DB_PATH}-wal"
    add_external_path "${DB_PATH}-shm"
    add_external_path "$TOKEN_PATH"
    log_path="$(read_log_path || true)"
    [[ -z "$log_path" ]] || add_external_path "$log_path"
}

new_snapshot_id() {
    local base candidate suffix=1
    base="$(date +%Y%m%d-%H%M%S)"
    candidate="$base"
    while [[ -e "$BACKUP_ROOT/$candidate" ]]; do
        candidate="${base}-${suffix}"
        suffix=$((suffix + 1))
    done
    printf '%s\n' "$candidate"
}

check_snapshot_space() {
    local required available install_kb external_kb=0 path
    mkdir -p -- "$BACKUP_ROOT"
    chmod 700 -- "$BACKUP_ROOT" 2>/dev/null || true
    install_kb="$(du -sk -- "$INSTALL_DIR" | awk '{print $1}')"
    collect_external_state
    for path in "${EXTERNAL_PATHS[@]}"; do
        external_kb=$((external_kb + $(du -sk -- "$path" | awk '{print $1}')))
    done
    required=$((install_kb + external_kb + 65536))
    available="$(df -Pk -- "$BACKUP_ROOT" | awk 'NR == 2 {print $4}')"
    ((available >= required)) ||
        die "Insufficient snapshot space in $BACKUP_ROOT"
}

create_snapshot() {
    local reason="$1"
    local was_active="$2"
    local was_enabled="$3"
    local identity snapshot_id snapshot_dir install_relative log_path unit_path
    [[ -d "$INSTALL_DIR" ]] || die "Install directory not found: $INSTALL_DIR"
    service_active && die "Refusing to snapshot a running SySeBa service"

    check_snapshot_space
    snapshot_id="$(new_snapshot_id)"
    snapshot_dir="$BACKUP_ROOT/$snapshot_id"
    mkdir -m 0700 -- "$snapshot_dir"
    install_relative="${INSTALL_DIR#/}"
    identity="$(installed_identity)"
    log_path="$(read_log_path || true)"
    unit_path="$(service_fragment || true)"

    log "Creating consistent snapshot ${snapshot_id}..."
    tar --xattrs --acls --numeric-owner \
        -C / -czpf "$snapshot_dir/install.tar.gz" \
        --exclude='*.lock' \
        -- "$install_relative"

    collect_external_state
    if ((${#EXTERNAL_PATHS[@]} > 0)); then
        local -a relative_paths=()
        local path
        for path in "${EXTERNAL_PATHS[@]}"; do
            relative_paths+=("${path#/}")
        done
        tar --xattrs --acls --numeric-owner \
            -C / -czpf "$snapshot_dir/external-state.tar.gz" \
            -- "${relative_paths[@]}"
        printf '%s\n' "${EXTERNAL_PATHS[@]}" \
            >"$snapshot_dir/external-state.paths"
    fi
    if [[ -n "$unit_path" && -f "$unit_path" ]]; then
        cp -a -- "$unit_path" "$snapshot_dir/service.unit"
    fi

    {
        printf 'snapshot_id=%s\n' "$snapshot_id"
        printf 'created_at=%s\n' "$(date --iso-8601=seconds)"
        printf 'reason=%s\n' "$reason"
        printf 'install_dir=%s\n' "$INSTALL_DIR"
        printf 'identity=%s\n' "$identity"
        printf 'service=%s\n' "$SERVICE_NAME"
        printf 'service_was_active=%s\n' "$was_active"
        printf 'service_was_enabled=%s\n' "$was_enabled"
        printf 'unit_path=%s\n' "$unit_path"
        printf 'config_path=%s\n' "$CONFIG_PATH"
        printf 'db_path=%s\n' "$DB_PATH"
        printf 'token_path=%s\n' "$TOKEN_PATH"
        printf 'log_path=%s\n' "$log_path"
    } >"$snapshot_dir/manifest.txt"

    (
        cd "$snapshot_dir"
        local -a files=(manifest.txt install.tar.gz)
        [[ ! -f service.unit ]] || files+=(service.unit)
        if [[ -f external-state.tar.gz ]]; then
            files+=(external-state.tar.gz external-state.paths)
        fi
        sha256sum -- "${files[@]}" >SHA256SUMS
    )
    printf '%s\n' "$snapshot_id" >"$BACKUP_ROOT/LATEST"
    if [[ "$reason" == "pre-update" ]]; then
        printf '%s\n' "$snapshot_id" >"$BACKUP_ROOT/LATEST_PRE_UPDATE"
    fi
    log "Snapshot ready: $snapshot_dir"
    printf '%s\n' "$snapshot_dir"
}

manifest_value() {
    local manifest="$1"
    local key="$2"
    awk -F= -v wanted="$key" '
        $1 == wanted {sub(/^[^=]*=/, ""); print; exit}
    ' "$manifest"
}

resolve_snapshot() {
    local selector="${1:-latest}"
    case "$selector" in
        latest)
            [[ -f "$BACKUP_ROOT/LATEST" ]] ||
                die "No latest snapshot marker found"
            selector="$(tr -d '\r\n' <"$BACKUP_ROOT/LATEST")"
            ;;
        pre-update)
            [[ -f "$BACKUP_ROOT/LATEST_PRE_UPDATE" ]] ||
                die "No pre-update snapshot marker found"
            selector="$(tr -d '\r\n' <"$BACKUP_ROOT/LATEST_PRE_UPDATE")"
            ;;
    esac
    [[ "$selector" =~ ^[0-9]{8}-[0-9]{6}(-[0-9]+)?$ ]] ||
        die "Invalid snapshot ID: $selector"
    [[ -d "$BACKUP_ROOT/$selector" ]] ||
        die "Snapshot not found: $selector"
    printf '%s\n' "$BACKUP_ROOT/$selector"
}

archive_members_safe() {
    local archive="$1"
    local required_prefix="${2:-}"
    local member listing unsafe=0
    listing="$(mktemp "${BACKUP_ROOT}/.archive-members.XXXXXX")" ||
        return 1
    if ! tar -tzf "$archive" >"$listing"; then
        rm -f -- "$listing"
        return 1
    fi
    while IFS= read -r member; do
        [[ -n "$member" ]] || continue
        [[ "$member" != /* &&
           "$member" != ../* &&
           "$member" != */../* &&
           "$member" != *'/..' ]] || {
            unsafe=1
            break
        }
        if [[ -n "$required_prefix" ]]; then
            [[ "$member" == "$required_prefix" ||
               "$member" == "$required_prefix"/* ]] || {
                unsafe=1
                break
            }
        fi
    done <"$listing"
    rm -f -- "$listing"
    ((unsafe == 0))
}

verify_snapshot() {
    local snapshot="$1"
    local archived_install prefix
    [[ -f "$snapshot/SHA256SUMS" &&
       -f "$snapshot/manifest.txt" &&
       -f "$snapshot/install.tar.gz" ]] ||
        die "Snapshot is incomplete: $snapshot"
    (cd "$snapshot" && sha256sum -c SHA256SUMS)
    archived_install="$(manifest_value "$snapshot/manifest.txt" install_dir)"
    [[ "$archived_install" == "$INSTALL_DIR" ]] ||
        die "Snapshot belongs to $archived_install, not $INSTALL_DIR"
    prefix="${INSTALL_DIR#/}"
    archive_members_safe "$snapshot/install.tar.gz" "$prefix" ||
        die "Unsafe path found in install archive"
    if [[ -f "$snapshot/external-state.tar.gz" ]]; then
        archive_members_safe "$snapshot/external-state.tar.gz" "" ||
            die "Unsafe path found in external-state archive"
    fi
}

restore_external_state() {
    local snapshot="$1"
    [[ -f "$snapshot/external-state.tar.gz" ]] || return 0
    tar --xattrs --acls --numeric-owner \
        -C / -xzpf "$snapshot/external-state.tar.gz"
}

restore_unit() {
    local snapshot="$1"
    local unit_path
    unit_path="$(manifest_value "$snapshot/manifest.txt" unit_path)"
    rm -f -- "$UNIT_OVERRIDE"
    if [[ -f "$snapshot/service.unit" && "$unit_path" == /* ]]; then
        mkdir -p -- "$(dirname "$unit_path")"
        cp -a -- "$snapshot/service.unit" "$unit_path"
    fi
    systemctl daemon-reload
}

systemd_escape_argument() {
    local value="$1"
    value="${value//\\/\\\\}"
    value="${value//\"/\\\"}"
    value="${value//%/%%}"
    value="${value//\$/\$\$}"
    printf '%s' "$value"
}

write_native_unit() {
    local binary config db token lock temporary
    local source backup restore config_parent db_parent token_parent lock_parent
    local log_path log_parent
    binary="$(systemd_escape_argument "$INSTALL_DIR/syseba")"
    config="$(systemd_escape_argument "$CONFIG_PATH")"
    db="$(systemd_escape_argument "$DB_PATH")"
    token="$(systemd_escape_argument "$TOKEN_PATH")"
    lock="$(systemd_escape_argument "$LOCK_PATH")"
    source="$(read_config_value source)"
    backup="$(read_config_value backup)"
    restore="$(read_config_value restore)"
    log_path="$(read_log_path)"
    [[ "$source" == /* && "$backup" == /* && "$restore" == /* &&
       "$log_path" == /* ]] ||
        die "The service requires absolute source, backup, restore and log paths"
    source="$(systemd_escape_argument "$(normalize_path "$source")")"
    backup="$(systemd_escape_argument "$(normalize_path "$backup")")"
    restore="$(systemd_escape_argument "$(normalize_path "$restore")")"
    config_parent="$(systemd_escape_argument "$(dirname "$CONFIG_PATH")")"
    db_parent="$(systemd_escape_argument "$(dirname "$DB_PATH")")"
    token_parent="$(systemd_escape_argument "$(dirname "$TOKEN_PATH")")"
    lock_parent="$(systemd_escape_argument "$(dirname "$LOCK_PATH")")"
    log_parent="$(systemd_escape_argument "$(dirname "$log_path")")"
    temporary="${UNIT_OVERRIDE}.tmp.$$"
    mkdir -p -- "$(dirname "$UNIT_OVERRIDE")"
    cat >"$temporary" <<EOF
[Unit]
Description=SySeBa - The Syncro Service Backup
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
ExecStart="${binary}" run --silent --web --web-host ${WEB_HOST} --web-port ${WEB_PORT} --config "${config}" --lockfile "${lock}" --db-path "${db}" --web-token-file "${token}"
Restart=always
RestartSec=5
TimeoutStopSec=45
User=root
Group=root
StandardOutput=journal
StandardError=journal
SyslogIdentifier=syseba
NoNewPrivileges=true
CapabilityBoundingSet=CAP_DAC_OVERRIDE CAP_FOWNER
PrivateTmp=true
PrivateDevices=true
ProtectSystem=full
ReadWritePaths="${source}" "${backup}" "${restore}" "${config_parent}" "${token_parent}" "${db_parent}" "${lock_parent}" "${log_parent}"
ProtectProc=invisible
ProcSubset=pid
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
ProtectKernelLogs=true
ProtectClock=true
ProtectHostname=true
RestrictSUIDSGID=true
RestrictRealtime=true
RestrictNamespaces=true
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6
LockPersonality=true
MemoryDenyWriteExecute=true
RemoveIPC=true
SystemCallFilter=~@mount @module @reboot @swap @raw-io @debug @cpu-emulation
SystemCallErrorNumber=EPERM
SystemCallArchitectures=native
UMask=0077

[Install]
WantedBy=multi-user.target
EOF
    chmod 0644 "$temporary"
    mv -f -- "$temporary" "$UNIT_OVERRIDE"
}

copy_state_into_stage() {
    local stage="$1"
    local path relative target log_path
    local -a paths=(
        "$CONFIG_PATH"
        "$DB_PATH"
        "${DB_PATH}-wal"
        "${DB_PATH}-shm"
        "$TOKEN_PATH"
    )
    log_path="$(read_log_path || true)"
    [[ -z "$log_path" ]] || paths+=("$log_path")
    for path in "${paths[@]}"; do
        [[ -e "$path" || -L "$path" ]] || continue
        path_is_within "$INSTALL_DIR" "$path" || continue
        relative="${path#"$INSTALL_DIR"/}"
        [[ "$relative" != "$path" ]] || continue
        target="$stage/$relative"
        mkdir -p -- "$(dirname "$target")"
        cp -a -- "$path" "$target"
    done
}

source_commit() {
    local source="$1"
    if [[ -d "$source/.git" ]]; then
        git -C "$source" rev-parse HEAD 2>/dev/null || printf 'uncommitted\n'
    else
        printf 'source-archive\n'
    fi
}

build_candidate() {
    local source="$1"
    local build="$TEMP_ROOT/build"
    local -a generator=()
    [[ -f "$source/CMakeLists.txt" ]] ||
        die "CMakeLists.txt not found in release source"
    [[ -f "$source/src/main.c" ]] ||
        die "Native C source is incomplete"
    command_exists ninja && generator=(-G Ninja)

    log "Configuring native Release build..."
    cmake -S "$source" -B "$build" "${generator[@]}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSYSEBA_BUILD_TESTS=ON >&2
    log "Compiling SySeBa..."
    cmake --build "$build" --parallel "$(nproc)" >&2
    "$build/syseba" --version >&2
    ctest --test-dir "$build" --output-on-failure -R native-unit >&2
    if command_exists curl && command_exists python3; then
        ctest --test-dir "$build" --output-on-failure \
            -R linux-integration >&2
    fi
    "$build/syseba" config-check --config "$CONFIG_PATH" >&2
    printf '%s\n' "$build/syseba"
}

prepare_stage() {
    local source="$1"
    local binary="$2"
    local stage="$3"
    install -d -m 0750 "$stage"
    install -m 0755 "$binary" "$stage/syseba"
    install -m 0755 \
        "$source/scripts/syseba-maintenance.sh" \
        "$stage/syseba-maintenance.sh"
    local file
    for file in README.md README.it.md ReadmeAI.md ReadmeAI.en.md \
        THIRD_PARTY_NOTICES.md CHANGELOG.md LICENSE SySeBa_Logo.webp; do
        [[ ! -f "$source/$file" ]] ||
            install -m 0644 "$source/$file" "$stage/$file"
    done
    if [[ -d "$source/docs" ]]; then
        cp -a -- "$source/docs" "$stage/docs"
    fi
    {
        printf 'version=2.0.0\n'
        printf 'source_commit=%s\n' "$(source_commit "$source")"
        printf 'built_at=%s\n' "$(date --iso-8601=seconds)"
        printf 'compiler=%s\n' "$(cc --version | head -n 1)"
    } >"$stage/BUILD-INFO"
}

health_check() {
    local attempt
    for ((attempt = 1; attempt <= HEALTH_TIMEOUT; attempt++)); do
        if service_active; then
            if [[ -x "$INSTALL_DIR/syseba" ]] && command_exists curl; then
                if curl -fsS --max-time 2 \
                    "http://127.0.0.1:${WEB_PORT}/api/auth" \
                    >/dev/null 2>&1; then
                    log "Service and Web API health checks passed"
                    return 0
                fi
            else
                sleep 2
                service_active && {
                    log "Service health check passed"
                    return 0
                }
            fi
        fi
        sleep 1
    done
    journalctl -u "$SERVICE_NAME" -n 60 --no-pager >&2 || true
    return 1
}

rollback_failed_install() {
    local previous_dir="$1"
    local snapshot="$2"
    local failed_dir="$3"
    warn "Health check failed; restoring the exact previous installation"
    systemctl stop "$SERVICE_NAME" >/dev/null 2>&1 || true
    [[ ! -d "$INSTALL_DIR" ]] || mv -- "$INSTALL_DIR" "$failed_dir"
    mv -- "$previous_dir" "$INSTALL_DIR"
    restore_external_state "$snapshot"
    restore_unit "$snapshot"
    rm -f -- "$LOCK_PATH" "$INSTALL_DIR"/*.lock 2>/dev/null || true
    if [[ "$(manifest_value "$snapshot/manifest.txt" service_was_active)" == "1" ]]; then
        start_service
        health_check || warn "Previous version was restored but needs manual service inspection"
    fi
}

install_source() {
    local source="$1"
    local binary stage was_active=0 was_enabled=0 snapshot previous failed
    [[ -d "$INSTALL_DIR" ]] || die "Install directory not found: $INSTALL_DIR"
    [[ -f "$CONFIG_PATH" ]] ||
        die "Configuration not found: $CONFIG_PATH"
    require_command cmake
    require_command ctest
    require_command cc
    require_command tar
    require_command sha256sum
    require_command systemctl

    if [[ -z "$TEMP_ROOT" ]]; then
        TEMP_ROOT="$(mktemp -d \
            "$(dirname "$INSTALL_DIR")/.syseba-maintenance.XXXXXX")"
    fi
    binary="$(build_candidate "$source")"
    stage="$TEMP_ROOT/install"
    prepare_stage "$source" "$binary" "$stage"
    service_active && was_active=1
    service_enabled && was_enabled=1
    stop_service
    snapshot="$(create_snapshot pre-update "$was_active" "$was_enabled")"
    copy_state_into_stage "$stage"
    if path_is_within "$INSTALL_DIR" "$CONFIG_PATH"; then
        local staged_config
        staged_config="$stage/${CONFIG_PATH#"$INSTALL_DIR"/}"
        [[ -f "$staged_config" ]] ||
            die "The staged installation does not contain its configuration"
    fi

    previous="$(dirname "$INSTALL_DIR")/.syseba-previous-$(basename "$snapshot")"
    failed="$(dirname "$INSTALL_DIR")/.syseba-failed-$(basename "$snapshot")"
    [[ ! -e "$previous" && ! -e "$failed" ]] ||
        die "A previous or failed directory with this snapshot ID already exists"

    log "Switching to the verified native installation..."
    mv -- "$INSTALL_DIR" "$previous"
    mv -- "$stage" "$INSTALL_DIR"
    write_native_unit
    rm -f -- "$LOCK_PATH" "$INSTALL_DIR"/*.lock 2>/dev/null || true
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME" >/dev/null
    if ! "$INSTALL_DIR/syseba" config-check --config "$CONFIG_PATH" ||
        ! start_service ||
        ! health_check; then
        rollback_failed_install "$previous" "$snapshot" "$failed"
        die "Native installation failed; automatic rollback completed"
    fi

    printf 'Native SySeBa installation completed.\n'
    printf 'Installed:          %s\n' "$(installed_identity)"
    printf 'Rollback snapshot:  %s\n' "$snapshot"
    printf 'Previous directory: %s\n' "$previous"
    printf 'Web UI:             http://<server-ip>:%s\n' "$WEB_PORT"
    printf 'Token:              %s\n' "$TOKEN_PATH"
}

validate_git_ref() {
    local ref="$1"
    [[ "$ref" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$ &&
       "$ref" != *..* &&
       "$ref" != *'@{'* &&
       "$ref" != *'//'* ]] ||
        die "Invalid Git ref: $ref"
}

resolve_remote_commit() {
    local ref="$1"
    local line
    validate_git_ref "$ref"
    line="$(git ls-remote "$REPO_URL" \
        "refs/heads/$ref" "refs/tags/$ref^{}" "refs/tags/$ref" |
        head -n 1)"
    [[ "$line" =~ ^[0-9a-fA-F]{40}[[:space:]] ]] || return 1
    printf '%s\n' "${line%%[[:space:]]*}"
}

clone_release() {
    local ref="$1"
    local destination="$2"
    validate_git_ref "$ref"
    git clone --quiet --depth 1 --branch "$ref" "$REPO_URL" "$destination" ||
        {
            mkdir -p -- "$destination"
            git -C "$destination" init --quiet
            git -C "$destination" remote add origin "$REPO_URL"
            git -C "$destination" fetch --quiet --depth 1 origin "$ref"
            git -C "$destination" checkout --quiet --detach FETCH_HEAD
        }
}

cmd_install_local() {
    [[ -f "$SOURCE_ROOT/CMakeLists.txt" ]] ||
        die "install-local must be run from the SySeBa source checkout"
    install_source "$SOURCE_ROOT"
}

cmd_update() {
    local ref="${1:-$DEFAULT_REF}"
    TEMP_ROOT="$(mktemp -d "$(dirname "$INSTALL_DIR")/.syseba-maintenance.XXXXXX")"
    clone_release "$ref" "$TEMP_ROOT/source"
    install_source "$TEMP_ROOT/source"
}

cmd_quick_update() {
    local ref="${1:-$DEFAULT_REF}"
    local remote installed
    remote="$(resolve_remote_commit "$ref")" ||
        die "Unable to resolve ${ref} from ${REPO_URL}"
    installed="$(awk -F= '$1 == "source_commit" {print $2; exit}' \
        "$INSTALL_DIR/BUILD-INFO" 2>/dev/null || true)"
    printf 'Installed revision: %s\n' "${installed:-legacy-or-unknown}"
    printf 'Remote revision:    %s\n' "$remote"
    if [[ "$installed" == "$remote" ]]; then
        log "The installed source revision is current"
        cmd_verify
        return 0
    fi
    cmd_update "$remote"
}

cmd_backup() {
    local was_active=0 was_enabled=0 snapshot
    service_active && was_active=1
    service_enabled && was_enabled=1
    stop_service
    snapshot="$(create_snapshot manual "$was_active" "$was_enabled")"
    if ((was_active)); then
        start_service
        health_check || die "Snapshot succeeded but the service did not recover"
    fi
    printf 'Snapshot completed: %s\n' "$snapshot"
}

list_snapshots_table() {
    local directory manifest id created reason identity
    printf '%-18s %-25s %-12s %s\n' \
        "SNAPSHOT ID" "CREATED" "REASON" "IDENTITY"
    [[ -d "$BACKUP_ROOT" ]] || return 0
    while IFS= read -r directory; do
        manifest="$directory/manifest.txt"
        [[ -f "$manifest" ]] || continue
        id="$(basename "$directory")"
        created="$(manifest_value "$manifest" created_at)"
        reason="$(manifest_value "$manifest" reason)"
        identity="$(manifest_value "$manifest" identity)"
        printf '%-18s %-25.25s %-12.12s %s\n' \
            "$id" "$created" "$reason" "$identity"
    done < <(find "$BACKUP_ROOT" -mindepth 1 -maxdepth 1 \
        -type d -name '20*' -print | sort -r)
}

select_snapshot() {
    [[ -t 0 ]] ||
        die "Interactive rollback needs a terminal; pass a snapshot ID"
    local -a snapshots=()
    local directory choice index
    while IFS= read -r directory; do
        [[ -f "$directory/manifest.txt" ]] && snapshots+=("$directory")
    done < <(find "$BACKUP_ROOT" -mindepth 1 -maxdepth 1 \
        -type d -name '20*' -print | sort -r)
    ((${#snapshots[@]} > 0)) || die "No snapshots found"

    list_snapshots_table
    while true; do
        printf '\nSelect snapshot [1-%d], or q: ' "${#snapshots[@]}"
        IFS= read -r choice || return 1
        [[ "$choice" != q && "$choice" != Q ]] || return 1
        if [[ "$choice" =~ ^[1-9][0-9]{0,3}$ ]]; then
            index=$((10#$choice - 1))
            if ((index >= 0 && index < ${#snapshots[@]})); then
                printf '%s\n' "${snapshots[$index]}"
                return 0
            fi
        fi
        warn "Invalid selection"
    done
}

recover_current_after_failed_rollback() {
    local current_dir="$1"
    local current_snapshot="$2"
    local failed_dir="$3"
    systemctl stop "$SERVICE_NAME" >/dev/null 2>&1 || true
    [[ ! -d "$INSTALL_DIR" ]] || mv -- "$INSTALL_DIR" "$failed_dir"
    mv -- "$current_dir" "$INSTALL_DIR"
    restore_external_state "$current_snapshot"
    restore_unit "$current_snapshot"
    rm -f -- "$LOCK_PATH" "$INSTALL_DIR"/*.lock 2>/dev/null || true
    if [[ "$(manifest_value "$current_snapshot/manifest.txt" service_was_active)" == "1" ]]; then
        start_service || true
    fi
}

cmd_rollback() {
    local selector="${1:-}"
    local selected current_active=0 current_enabled=0 current_snapshot
    local quarantine failed selected_active selected_enabled
    if [[ -z "$selector" ]]; then
        selected="$(select_snapshot)" || {
            printf 'Rollback canceled; no files were changed.\n'
            return 0
        }
    else
        selected="$(resolve_snapshot "$selector")"
    fi
    verify_snapshot "$selected"
    printf 'Selected: %s\n' "$selected"
    printf 'Identity: %s\n' \
        "$(manifest_value "$selected/manifest.txt" identity)"
    if [[ -t 0 ]]; then
        printf 'Restore this exact snapshot? [y/N]: '
        local confirmation
        IFS= read -r confirmation || return 0
        [[ "$confirmation" == y || "$confirmation" == Y ||
           "$confirmation" == yes || "$confirmation" == YES ]] ||
            {
                printf 'Rollback canceled; no files were changed.\n'
                return 0
            }
    fi

    service_active && current_active=1
    service_enabled && current_enabled=1
    stop_service
    current_snapshot="$(create_snapshot pre-rollback \
        "$current_active" "$current_enabled")"
    quarantine="$(dirname "$INSTALL_DIR")/.syseba-before-rollback-$(date +%s)"
    failed="$(dirname "$INSTALL_DIR")/.syseba-rollback-failed-$(date +%s)"
    mv -- "$INSTALL_DIR" "$quarantine"
    if ! tar --xattrs --acls --numeric-owner \
        -C / -xzpf "$selected/install.tar.gz"; then
        mv -- "$quarantine" "$INSTALL_DIR"
        die "Snapshot extraction failed; current installation restored"
    fi
    restore_external_state "$selected"
    restore_unit "$selected"
    rm -f -- "$LOCK_PATH" "$INSTALL_DIR"/*.lock 2>/dev/null || true
    selected_active="$(manifest_value "$selected/manifest.txt" service_was_active)"
    selected_enabled="$(manifest_value "$selected/manifest.txt" service_was_enabled)"
    [[ "$selected_enabled" != "1" ]] ||
        systemctl enable "$SERVICE_NAME" >/dev/null 2>&1 || true
    if [[ "$selected_active" == "1" ]]; then
        if ! start_service || ! health_check; then
            recover_current_after_failed_rollback \
                "$quarantine" "$current_snapshot" "$failed"
            die "Selected snapshot failed health checks; current version restored"
        fi
    fi
    printf 'Rollback completed from: %s\n' "$selected"
    printf 'Replaced installation retained at: %s\n' "$quarantine"
}

cmd_list() {
    list_snapshots_table
}

cmd_verify() {
    local binary
    if [[ -x "$INSTALL_DIR/syseba" ]]; then
        binary="$INSTALL_DIR/syseba"
        "$binary" --version
        "$binary" config-check --config "$CONFIG_PATH"
    elif [[ -f "$INSTALL_DIR/syseba.py" ]]; then
        python3 -m py_compile "$INSTALL_DIR/syseba.py"
        python3 "$INSTALL_DIR/syseba.py" config-check \
            --config "$CONFIG_PATH"
    else
        die "No SySeBa executable found in $INSTALL_DIR"
    fi
    systemctl status "$SERVICE_NAME" --no-pager --lines=10 || true
    if service_active && command_exists curl; then
        curl -fsS --max-time 3 \
            "http://127.0.0.1:${WEB_PORT}/api/auth" >/dev/null ||
            die "Web API health endpoint is not reachable"
    fi
    printf 'Verification completed: %s\n' "$(installed_identity)"
}

cmd_logs() {
    local lines="${1:-100}"
    local log_path
    if [[ ! "$lines" =~ ^[1-9][0-9]{0,3}$ ]] ||
        ((10#$lines > 2000)); then
        die "Log line count must be between 1 and 2000"
    fi
    printf '\n=== SYSTEMD: %s ===\n' "$SERVICE_NAME"
    journalctl -u "$SERVICE_NAME" -n "$lines" \
        --no-pager -o short-iso-precise || true
    log_path="$(read_log_path || true)"
    printf '\n=== SYSEBA APPLICATION: %s ===\n' \
        "${log_path:-not configured}"
    if [[ -n "$log_path" && -f "$log_path" ]]; then
        tail -n "$lines" -- "$log_path"
    fi
}

cmd_follow() {
    local log_path
    log_path="$(read_log_path || true)"
    printf 'Following SySeBa logs. Press Ctrl+C to stop.\n'
    if [[ -n "$log_path" && -f "$log_path" ]]; then
        tail -n 30 -F -- "$log_path" &
        FOLLOW_PID=$!
    fi
    journalctl --follow -u "$SERVICE_NAME" -n 30 \
        -o short-iso-precise
}

main() {
    local command="${1:-help}"
    shift || true
    case "$command" in
        help|-h|--help)
            usage
            return 0
            ;;
        install-local|quick-update|update|backup|rollback|list|verify|logs|follow)
            ;;
        *)
            usage >&2
            die "Unknown command: $command"
            ;;
    esac
    case "$command" in
        quick-update|update|rollback|logs)
            (($# <= 1)) || die "Too many arguments for $command"
            ;;
        *)
            (($# == 0)) || die "$command does not accept arguments"
            ;;
    esac

    validate_settings
    require_root
    require_command systemctl
    require_command tar
    require_command sha256sum
    case "$command" in
        install-local|quick-update|update|backup|rollback)
            acquire_lock
            ;;
    esac

    case "$command" in
        install-local) cmd_install_local ;;
        quick-update) cmd_quick_update "${1:-$DEFAULT_REF}" ;;
        update) cmd_update "${1:-$DEFAULT_REF}" ;;
        backup) cmd_backup ;;
        rollback) cmd_rollback "${1:-}" ;;
        list) cmd_list ;;
        verify) cmd_verify ;;
        logs) cmd_logs "${1:-100}" ;;
        follow) cmd_follow ;;
    esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
