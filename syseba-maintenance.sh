#!/usr/bin/env bash

set -Eeuo pipefail
IFS=$'\n\t'

SCRIPT_NAME="$(basename "$0")"
SCRIPT_VERSION="1.2"
LAUNCH_DIR="$(pwd -P)"

INSTALL_DIR="${SYSEBA_INSTALL_DIR:-/opt/syseba}"
BACKUP_ROOT="${SYSEBA_BACKUP_ROOT:-${LAUNCH_DIR}/syseba-backups}"
MAINTENANCE_LOCK="${SYSEBA_MAINTENANCE_LOCK:-${BACKUP_ROOT}/.maintenance.lock}"
SERVICE_NAME="${SYSEBA_SERVICE:-syseba.service}"
UNIT_FILE="${SYSEBA_UNIT_FILE:-/etc/systemd/system/${SERVICE_NAME}}"
REPO_URL="${SYSEBA_REPO_URL:-https://github.com/okno/SySeBa.git}"
DEFAULT_REF="${SYSEBA_REF:-main}"
CONFIG_PATH="${SYSEBA_CONFIG_PATH:-${INSTALL_DIR}/syseba.conf}"
DB_PATH="${SYSEBA_DB_PATH:-${INSTALL_DIR}/syseba_logs.db}"
TOKEN_PATH="${SYSEBA_TOKEN_PATH:-${INSTALL_DIR}/syseba_web.token}"
WEB_HOST="${SYSEBA_WEB_HOST:-0.0.0.0}"
WEB_PORT="${SYSEBA_WEB_PORT:-8765}"
APP_LANGUAGE="${SYSEBA_LANG:-it}"
SYSTEMCTL_BIN="${SYSEBA_SYSTEMCTL_BIN:-systemctl}"
GIT_BIN="${SYSEBA_GIT_BIN:-git}"
TAR_BIN="${SYSEBA_TAR_BIN:-tar}"
PYTHON_BIN="${SYSEBA_PYTHON_BIN:-/usr/bin/python3}"
HEALTH_WAIT="${SYSEBA_HEALTH_WAIT:-3}"

TEMP_STAGE=""
EXIT_RESTART_SERVICE=0
FOLLOW_PID=""
SELECTED_BACKUP=""

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
SySeBa maintenance tool ${SCRIPT_VERSION}

Usage:
  ${SCRIPT_NAME} quick-update [branch-tag-or-commit]
  ${SCRIPT_NAME} backup
  ${SCRIPT_NAME} update [branch-tag-or-commit]
  ${SCRIPT_NAME} rollback [backup-id|latest|pre-update]
  ${SCRIPT_NAME} list
  ${SCRIPT_NAME} verify
  ${SCRIPT_NAME} logs [lines]
  ${SCRIPT_NAME} follow

Commands:
  quick-update  Compare local and remote versions, then back up, update, restart
                and print startup logs. This is the recommended command.
  backup    Stop SySeBa if active, create a consistent snapshot, then restart it.
  update    Back up the current install, clone Git into staging, verify and switch.
            If the new service does not stay active, the old directory is restored.
  rollback  Without an argument, interactively select an exact snapshot to restore.
            With an ID, restore it directly. The current install is quarantined.
  list      List available snapshots.
  verify    Compile and validate the currently installed version and configuration.
  logs      Print recent systemd and SySeBa application logs.
  follow    Follow systemd and application logs together until Ctrl+C.

Default locations:
  install:  ${INSTALL_DIR}
  backups:  ${BACKUP_ROOT} (relative to the launch directory by default)
  service:  ${SERVICE_NAME}
  repo:     ${REPO_URL}

Environment overrides:
  SYSEBA_INSTALL_DIR, SYSEBA_BACKUP_ROOT, SYSEBA_SERVICE,
  SYSEBA_MAINTENANCE_LOCK,
  SYSEBA_UNIT_FILE, SYSEBA_REPO_URL, SYSEBA_REF,
  SYSEBA_CONFIG_PATH, SYSEBA_DB_PATH, SYSEBA_TOKEN_PATH,
  SYSEBA_WEB_HOST, SYSEBA_WEB_PORT, SYSEBA_LANG,
  SYSEBA_PYTHON_BIN, SYSEBA_SYSTEMCTL_BIN, SYSEBA_HEALTH_WAIT

The source, backup and restore data trees configured in syseba.conf are never copied,
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
    local parent child
    parent="$(normalize_path "$1")"
    child="$(normalize_path "$2")"
    [[ "$child" == "$parent" || "$child" == "$parent"/* ]]
}

validate_settings() {
    require_command readlink
    INSTALL_DIR="$(normalize_path "$INSTALL_DIR")"
    BACKUP_ROOT="$(normalize_path "$BACKUP_ROOT")"
    MAINTENANCE_LOCK="$(normalize_path "$MAINTENANCE_LOCK")"
    UNIT_FILE="$(normalize_path "$UNIT_FILE")"
    CONFIG_PATH="$(normalize_path "$CONFIG_PATH")"
    DB_PATH="$(normalize_path "$DB_PATH")"
    TOKEN_PATH="$(normalize_path "$TOKEN_PATH")"

    [[ "$INSTALL_DIR" == /* && "$INSTALL_DIR" != "/" ]] || die "Unsafe install path: $INSTALL_DIR"
    [[ "$BACKUP_ROOT" == /* && "$BACKUP_ROOT" != "/" ]] || die "Unsafe backup path: $BACKUP_ROOT"
    [[ "$MAINTENANCE_LOCK" == /* && "$MAINTENANCE_LOCK" != "/" ]] || die "Unsafe maintenance lock path: $MAINTENANCE_LOCK"
    [[ "$UNIT_FILE" == /* && "$UNIT_FILE" != "/" ]] || die "Unsafe unit path: $UNIT_FILE"
    [[ "$SERVICE_NAME" =~ ^[A-Za-z0-9_.@-]+$ ]] || die "Invalid systemd service name: $SERVICE_NAME"
    [[ "$HEALTH_WAIT" =~ ^[0-9]+$ ]] || die "SYSEBA_HEALTH_WAIT must be a non-negative integer"
    [[ "$WEB_HOST" =~ ^[A-Za-z0-9._:-]+$ ]] || die "Invalid Web listen address: $WEB_HOST"
    [[ "$WEB_PORT" =~ ^[1-9][0-9]{0,4}$ ]] || die "SYSEBA_WEB_PORT must be between 1 and 65535"
    (( 10#$WEB_PORT <= 65535 )) || die "SYSEBA_WEB_PORT must be between 1 and 65535"
    [[ "$APP_LANGUAGE" == "it" || "$APP_LANGUAGE" == "en" ]] || die "SYSEBA_LANG must be it or en"
}

validate_backup_location() {
    if path_is_within "$INSTALL_DIR" "$BACKUP_ROOT"; then
        die "Backup root cannot be inside the SySeBa install directory."
    fi
}

require_root() {
    if [[ "${EUID}" -ne 0 && "${SYSEBA_ALLOW_NON_ROOT:-0}" != "1" ]]; then
        die "Run this command as root (sudo $SCRIPT_NAME ...)."
    fi
}

resolve_tools() {
    require_command "$SYSTEMCTL_BIN"
    require_command "$GIT_BIN"
    require_command "$TAR_BIN"
    require_command sha256sum
    require_command mktemp
    require_command "$PYTHON_BIN"
}

acquire_maintenance_lock() {
    require_command flock
    mkdir -p -- "$(dirname "$MAINTENANCE_LOCK")"
    exec 9>"$MAINTENANCE_LOCK"
    flock -n 9 || die "Another SySeBa maintenance operation is already running."
}

service_is_active() {
    "$SYSTEMCTL_BIN" is-active --quiet "$SERVICE_NAME" >/dev/null 2>&1
}

running_syseba_pids() {
    if command_exists pgrep; then
        pgrep -f -- "${INSTALL_DIR}/syseba.py" 2>/dev/null || true
    fi
}

ensure_no_unmanaged_process() {
    local pids
    pids="$(running_syseba_pids)"
    if [[ -n "$pids" ]]; then
        die "SySeBa is still running outside the stopped service (PID: ${pids//$'\n'/, }). Stop it before continuing."
    fi
}

stop_service() {
    log "Stopping ${SERVICE_NAME}..."
    "$SYSTEMCTL_BIN" stop "$SERVICE_NAME"
    local attempt
    for attempt in $(seq 1 30); do
        if ! service_is_active; then
            ensure_no_unmanaged_process
            return 0
        fi
        sleep 1
    done
    die "${SERVICE_NAME} did not stop within 30 seconds."
}

show_recent_service_log() {
    if command_exists journalctl; then
        journalctl -u "$SERVICE_NAME" -n 25 --no-pager >&2 || true
    fi
}

start_service() {
    log "Starting ${SERVICE_NAME}..."
    "$SYSTEMCTL_BIN" daemon-reload
    if ! "$SYSTEMCTL_BIN" start "$SERVICE_NAME"; then
        warn "systemctl could not start ${SERVICE_NAME}."
        show_recent_service_log
        return 1
    fi

    local attempt
    for attempt in $(seq 1 30); do
        if service_is_active; then
            if (( HEALTH_WAIT > 0 )); then
                sleep "$HEALTH_WAIT"
            fi
            if service_is_active; then
                log "${SERVICE_NAME} is active."
                return 0
            fi
            break
        fi
        sleep 1
    done

    warn "${SERVICE_NAME} is not active after startup."
    show_recent_service_log
    return 1
}

cleanup_stage() {
    [[ -n "$TEMP_STAGE" && -e "$TEMP_STAGE" ]] || return 0
    local parent name expected_parent
    parent="$(dirname "$TEMP_STAGE")"
    name="$(basename "$TEMP_STAGE")"
    expected_parent="$(dirname "$INSTALL_DIR")"
    if [[ "$parent" == "$expected_parent" && "$name" == .syseba-stage.* ]]; then
        rm -rf -- "$TEMP_STAGE"
    else
        warn "Refusing to remove unexpected staging path: $TEMP_STAGE"
    fi
    TEMP_STAGE=""
}

on_exit() {
    local status=$?
    if [[ -n "$FOLLOW_PID" ]] && kill -0 "$FOLLOW_PID" 2>/dev/null; then
        kill "$FOLLOW_PID" 2>/dev/null || true
        wait "$FOLLOW_PID" 2>/dev/null || true
    fi
    cleanup_stage
    if [[ "$status" -ne 0 && "$EXIT_RESTART_SERVICE" -eq 1 && -d "$INSTALL_DIR" ]]; then
        warn "An operation failed while the original install was stopped; attempting restart."
        start_service || true
    fi
}

trap on_exit EXIT

read_log_path() {
    [[ -f "$CONFIG_PATH" ]] || return 0
    "$PYTHON_BIN" - "$CONFIG_PATH" <<'PY'
import configparser
import os
import sys

path = os.path.abspath(sys.argv[1])
parser = configparser.ConfigParser(interpolation=None)
try:
    parser.read(path, encoding="utf-8-sig")
    value = parser.get("SETTINGS", "log", fallback="").strip()
except (OSError, configparser.Error):
    value = ""
if value:
    if not os.path.isabs(value):
        value = os.path.join(os.path.dirname(path), value)
    print(os.path.abspath(os.path.expanduser(value)))
PY
}

new_backup_id() {
    local base candidate suffix
    base="$(date +%Y%m%d-%H%M%S)"
    candidate="$base"
    suffix=1
    while [[ -e "$BACKUP_ROOT/$candidate" ]]; do
        candidate="${base}-${suffix}"
        suffix=$((suffix + 1))
    done
    printf '%s\n' "$candidate"
}

git_commit_for() {
    local directory="$1"
    if [[ -d "$directory/.git" ]]; then
        "$GIT_BIN" -C "$directory" rev-parse HEAD 2>/dev/null || printf 'unknown\n'
    else
        printf 'not-a-git-checkout\n'
    fi
}

validate_git_ref() {
    local ref="$1"
    [[ "$ref" =~ ^[A-Za-z0-9][A-Za-z0-9._/-]{0,199}$ ]] || die "Invalid Git ref: $ref"
    [[ "$ref" != *..* && "$ref" != *'@{'* && "$ref" != *'//'* ]] || die "Invalid Git ref: $ref"
    [[ "$ref" != */ && "$ref" != *. && "$ref" != */.* && "$ref" != *./* ]] || die "Invalid Git ref: $ref"
}

installed_identity() {
    local commit digest tracked_changes
    if [[ -d "$INSTALL_DIR/.git" ]] && commit="$("$GIT_BIN" -C "$INSTALL_DIR" rev-parse HEAD 2>/dev/null)"; then
        tracked_changes="$("$GIT_BIN" -C "$INSTALL_DIR" status --porcelain --untracked-files=no 2>/dev/null || true)"
        if [[ -n "$tracked_changes" ]]; then
            printf '%s-dirty\n' "$commit"
        else
            printf '%s\n' "$commit"
        fi
        return 0
    fi
    if [[ -f "$INSTALL_DIR/syseba.py" ]]; then
        digest="$(sha256sum "$INSTALL_DIR/syseba.py" | awk '{print $1}')"
        printf 'legacy-sha256:%s\n' "$digest"
        return 0
    fi
    printf 'not-installed\n'
}

resolve_target_commit() {
    local ref="$1"
    local remote_ref result
    validate_git_ref "$ref"

    if [[ "$ref" =~ ^[0-9a-fA-F]{40}$ ]]; then
        printf '%s\n' "${ref,,}"
        return 0
    fi

    log "Checking remote version for ${ref}..."
    for remote_ref in "refs/heads/${ref}" "refs/tags/${ref}^{}" "refs/tags/${ref}"; do
        if result="$("$GIT_BIN" ls-remote --exit-code "$REPO_URL" "$remote_ref" 2>/dev/null)"; then
            awk 'NR == 1 { print tolower($1); exit }' <<< "$result"
            return 0
        fi
    done
    return 1
}

add_external_path() {
    local candidate="$1"
    [[ -e "$candidate" || -L "$candidate" ]] || return 0
    path_is_within "$INSTALL_DIR" "$candidate" && return 0
    local existing
    for existing in "${EXTERNAL_PATHS[@]:-}"; do
        [[ "$existing" == "$candidate" ]] && return 0
    done
    EXTERNAL_PATHS+=("$candidate")
}

check_backup_capacity() {
    require_command df
    require_command du
    mkdir -p -- "$BACKUP_ROOT"
    chmod 700 "$BACKUP_ROOT" 2>/dev/null || true

    local total_kb available_kb required_kb path path_kb log_path
    total_kb="$(du -sk -- "$INSTALL_DIR" | awk 'NR == 1 { print $1 }')"
    [[ "$total_kb" =~ ^[0-9]+$ ]] || die "Unable to estimate the install size."

    EXTERNAL_PATHS=()
    add_external_path "$CONFIG_PATH"
    add_external_path "$TOKEN_PATH"
    add_external_path "$DB_PATH"
    add_external_path "${DB_PATH}-wal"
    add_external_path "${DB_PATH}-shm"
    log_path="$(read_log_path || true)"
    [[ -z "$log_path" ]] || add_external_path "$log_path"
    for path in "${EXTERNAL_PATHS[@]:-}"; do
        [[ -n "$path" ]] || continue
        path_kb="$(du -sk -- "$path" | awk 'NR == 1 { print $1 }')"
        [[ "$path_kb" =~ ^[0-9]+$ ]] || die "Unable to estimate backup size for $path"
        total_kb=$((total_kb + path_kb))
    done

    available_kb="$(df -Pk -- "$BACKUP_ROOT" | awk 'NR == 2 { print $4 }')"
    [[ "$available_kb" =~ ^[0-9]+$ ]] || die "Unable to determine free space in $BACKUP_ROOT"
    required_kb=$((total_kb + (total_kb / 20) + 65536))
    if (( available_kb < required_kb )); then
        die "Insufficient backup space: need about $((required_kb / 1024)) MiB, available $((available_kb / 1024)) MiB in $BACKUP_ROOT"
    fi
    log "Backup space check passed: about $((required_kb / 1024)) MiB required, $((available_kb / 1024)) MiB available."
}

create_backup() {
    local reason="$1"
    local service_was_active="$2"
    [[ -d "$INSTALL_DIR" ]] || die "Install directory not found: $INSTALL_DIR"
    [[ -f "$INSTALL_DIR/syseba.py" ]] || die "syseba.py not found in $INSTALL_DIR"

    mkdir -p -- "$BACKUP_ROOT"
    chmod 700 "$BACKUP_ROOT" 2>/dev/null || true

    local backup_id backup_dir install_relative log_path
    backup_id="$(new_backup_id)"
    backup_dir="$BACKUP_ROOT/$backup_id"
    mkdir -- "$backup_dir"
    chmod 700 "$backup_dir" 2>/dev/null || true
    install_relative="${INSTALL_DIR#/}"

    log "Creating stopped-state application snapshot ${backup_id}..."
    "$TAR_BIN" -C / \
        --exclude='*.lock' \
        --exclude='__pycache__' \
        -czf "$backup_dir/syseba-app.tar.gz" \
        -- "$install_relative"

    if [[ -e "$UNIT_FILE" || -L "$UNIT_FILE" ]]; then
        cp -a -- "$UNIT_FILE" "$backup_dir/syseba.service"
    fi

    EXTERNAL_PATHS=()
    add_external_path "$CONFIG_PATH"
    add_external_path "$TOKEN_PATH"
    add_external_path "$DB_PATH"
    add_external_path "${DB_PATH}-wal"
    add_external_path "${DB_PATH}-shm"
    log_path="$(read_log_path || true)"
    if [[ -n "$log_path" ]]; then
        add_external_path "$log_path"
    fi

    if (( ${#EXTERNAL_PATHS[@]} > 0 )); then
        local external_relative=()
        local item
        for item in "${EXTERNAL_PATHS[@]}"; do
            external_relative+=("${item#/}")
        done
        "$TAR_BIN" -C / -czf "$backup_dir/external-state.tar.gz" -- "${external_relative[@]}"
        printf '%s\n' "${EXTERNAL_PATHS[@]}" > "$backup_dir/external-state.paths"
    fi

    cat > "$backup_dir/manifest.txt" <<EOF
backup_id=${backup_id}
created_at=$(date --iso-8601=seconds)
reason=${reason}
install_dir=${INSTALL_DIR}
service=${SERVICE_NAME}
service_was_active=${service_was_active}
git_commit=$(git_commit_for "$INSTALL_DIR")
config_path=${CONFIG_PATH}
db_path=${DB_PATH}
log_path=${log_path}
EOF

    (
        cd "$backup_dir"
        local checksum_files=(manifest.txt syseba-app.tar.gz)
        [[ -f syseba.service ]] && checksum_files+=(syseba.service)
        [[ -f external-state.tar.gz ]] && checksum_files+=(external-state.tar.gz external-state.paths)
        sha256sum -- "${checksum_files[@]}" > SHA256SUMS
    )

    printf '%s\n' "$backup_id" > "$BACKUP_ROOT/LATEST"
    if [[ "$reason" == "pre-update" ]]; then
        printf '%s\n' "$backup_id" > "$BACKUP_ROOT/LATEST_PRE_UPDATE"
    fi
    log "Snapshot ready: $backup_dir"
    log "Snapshot size: $(du -sh "$backup_dir" | awk '{print $1}')"
    printf '%s\n' "$backup_dir"
}

manifest_value() {
    local manifest="$1"
    local key="$2"
    awk -F= -v wanted="$key" '$1 == wanted {sub(/^[^=]*=/, ""); print; exit}' "$manifest"
}

resolve_backup() {
    local selector="${1:-latest}"
    if [[ "$selector" == "latest" ]]; then
        [[ -f "$BACKUP_ROOT/LATEST" ]] || die "No latest backup marker found in $BACKUP_ROOT"
        selector="$(tr -d '\r\n' < "$BACKUP_ROOT/LATEST")"
    elif [[ "$selector" == "pre-update" ]]; then
        [[ -f "$BACKUP_ROOT/LATEST_PRE_UPDATE" ]] || die "No pre-update backup marker found in $BACKUP_ROOT"
        selector="$(tr -d '\r\n' < "$BACKUP_ROOT/LATEST_PRE_UPDATE")"
    fi
    [[ "$selector" =~ ^[0-9]{8}-[0-9]{6}(-[0-9]+)?$ ]] || die "Invalid backup id: $selector"
    local backup_dir="$BACKUP_ROOT/$selector"
    [[ -d "$backup_dir" ]] || die "Backup not found: $backup_dir"
    printf '%s\n' "$backup_dir"
}

verify_backup() {
    local backup_dir="$1"
    [[ -f "$backup_dir/SHA256SUMS" ]] || die "Missing SHA256SUMS in $backup_dir"
    log "Verifying snapshot checksums..."
    (cd "$backup_dir" && sha256sum -c SHA256SUMS)

    local archived_install
    archived_install="$(manifest_value "$backup_dir/manifest.txt" install_dir)"
    [[ "$archived_install" == "$INSTALL_DIR" ]] || die "Backup belongs to $archived_install, not $INSTALL_DIR"
}

candidate_path_for() {
    local candidate_root="$1"
    local original_path="$2"
    if path_is_within "$INSTALL_DIR" "$original_path"; then
        if [[ "$original_path" == "$INSTALL_DIR" ]]; then
            printf '%s\n' "$candidate_root"
        else
            printf '%s/%s\n' "$candidate_root" "${original_path#"$INSTALL_DIR"/}"
        fi
    else
        printf '%s\n' "$original_path"
    fi
}

copy_runtime_file() {
    local candidate_root="$1"
    local source_path="$2"
    [[ -e "$source_path" || -L "$source_path" ]] || return 0
    path_is_within "$INSTALL_DIR" "$source_path" || return 0
    [[ "$source_path" != "$INSTALL_DIR" ]] || return 0
    local relative target
    relative="${source_path#"$INSTALL_DIR"/}"
    target="$candidate_root/$relative"
    mkdir -p -- "$(dirname "$target")"
    cp -a -- "$source_path" "$target"
}

copy_runtime_state() {
    local candidate_root="$1"
    local log_path exclude_file pattern
    copy_runtime_file "$candidate_root" "$CONFIG_PATH"
    copy_runtime_file "$candidate_root" "$TOKEN_PATH"
    copy_runtime_file "$candidate_root" "$DB_PATH"
    copy_runtime_file "$candidate_root" "${DB_PATH}-wal"
    copy_runtime_file "$candidate_root" "${DB_PATH}-shm"
    log_path="$(read_log_path || true)"
    [[ -n "$log_path" ]] && copy_runtime_file "$candidate_root" "$log_path"

    local runtime_file
    shopt -s nullglob
    for runtime_file in "$INSTALL_DIR"/*.db "$INSTALL_DIR"/*.db-wal "$INSTALL_DIR"/*.db-shm "$INSTALL_DIR"/*.log; do
        copy_runtime_file "$candidate_root" "$runtime_file"
    done
    shopt -u nullglob

    rm -f -- "$candidate_root"/*.lock 2>/dev/null || true

    exclude_file="$candidate_root/.git/info/exclude"
    if [[ -f "$exclude_file" ]]; then
        for pattern in '*.db' '*.db-*' '*.log' '*.lock' 'syseba_web.token'; do
            grep -Fqx -- "$pattern" "$exclude_file" || printf '%s\n' "$pattern" >> "$exclude_file"
        done
    fi

    local candidate_config relative_config
    candidate_config="$(candidate_path_for "$candidate_root" "$CONFIG_PATH")"
    if path_is_within "$candidate_root" "$candidate_config" && [[ -f "$candidate_config" ]]; then
        relative_config="${candidate_config#"$candidate_root"/}"
        if "$GIT_BIN" -C "$candidate_root" ls-files --error-unmatch "$relative_config" >/dev/null 2>&1; then
            "$GIT_BIN" -C "$candidate_root" update-index --skip-worktree "$relative_config"
        fi
    fi
}

clone_release() {
    local destination="$1"
    local ref="$2"
    log "Cloning ${REPO_URL} (${ref}) into staging..."
    mkdir -- "$destination"
    "$GIT_BIN" -C "$destination" init --quiet
    "$GIT_BIN" -C "$destination" remote add origin "$REPO_URL"
    "$GIT_BIN" -C "$destination" fetch --depth 1 origin "$ref"
    "$GIT_BIN" -C "$destination" checkout --detach --quiet FETCH_HEAD
}

verify_release_code() {
    local directory="$1"
    [[ -f "$directory/syseba.py" ]] || die "Missing syseba.py in $directory"
    [[ -f "$directory/syseba_web.js" ]] || die "Missing syseba_web.js in $directory"
    [[ -f "$directory/requirements.txt" ]] || die "Missing requirements.txt in $directory"

    log "Checking Python syntax..."
    PYTHONPYCACHEPREFIX="${TEMP_STAGE:-/tmp}/pycache" "$PYTHON_BIN" -m py_compile "$directory/syseba.py"
    log "Checking Python dependencies..."
    "$PYTHON_BIN" -c 'import psutil, watchdog'
}

verify_install() {
    local directory="$1"
    local config config_output
    config="$(candidate_path_for "$directory" "$CONFIG_PATH")"

    verify_release_code "$directory"
    [[ -f "$config" ]] || die "Configuration not found for verification: $config"
    log "Validating SySeBa configuration..."
    if ! config_output="$("$PYTHON_BIN" "$directory/syseba.py" config-check --config "$config" --json 2>&1)"; then
        printf '%s\n' "$config_output" >&2
        die "SySeBa configuration validation failed."
    fi
    log "Verification passed (commit $(git_commit_for "$directory"))."
}

restore_unit_from_backup() {
    local backup_dir="$1"
    if [[ -f "$backup_dir/syseba.service" ]]; then
        mkdir -p -- "$(dirname "$UNIT_FILE")"
        cp -a -- "$backup_dir/syseba.service" "$UNIT_FILE"
    fi
    "$SYSTEMCTL_BIN" daemon-reload
}

configure_web_service() {
    local default_unit="/etc/systemd/system/syseba.service"
    if [[ "$SERVICE_NAME" != "syseba.service" || "$UNIT_FILE" != "$default_unit" ]]; then
        warn "Automatic Web service migration requires syseba.service at $default_unit."
        return 1
    fi

    local unit_backup had_unit=0
    unit_backup="$(mktemp)"
    if [[ -e "$UNIT_FILE" || -L "$UNIT_FILE" ]]; then
        cp -a -- "$UNIT_FILE" "$unit_backup"
        had_unit=1
    fi

    log "Configuring the systemd service with automatic Web UI on ${WEB_HOST}:${WEB_PORT}..."
    if ! "$PYTHON_BIN" "$INSTALL_DIR/syseba.py" service-install \
        --config "$CONFIG_PATH" \
        --lang "$APP_LANGUAGE" \
        --web-host "$WEB_HOST" \
        --web-port "$WEB_PORT" \
        --web-token-file "$TOKEN_PATH"; then
        warn "Unable to generate the Web-enabled systemd service."
        if [[ "$had_unit" -eq 1 ]]; then
            cp -a -- "$unit_backup" "$UNIT_FILE"
        else
            rm -f -- "$UNIT_FILE"
        fi
        "$SYSTEMCTL_BIN" daemon-reload || true
        rm -f -- "$unit_backup"
        return 1
    fi

    if ! grep -Eq -- '(^|[[:space:]])--web([[:space:]]|$)' "$UNIT_FILE" ||
        ! grep -Fq -- "--web-host ${WEB_HOST}" "$UNIT_FILE" ||
        ! grep -Fq -- "--web-port ${WEB_PORT}" "$UNIT_FILE" ||
        ! grep -Fq -- "--web-token-file ${TOKEN_PATH}" "$UNIT_FILE" ||
        [[ ! -s "$TOKEN_PATH" ]]; then
        warn "The generated service did not pass Web autostart validation."
        if [[ "$had_unit" -eq 1 ]]; then
            cp -a -- "$unit_backup" "$UNIT_FILE"
        else
            rm -f -- "$UNIT_FILE"
        fi
        "$SYSTEMCTL_BIN" daemon-reload || true
        rm -f -- "$unit_backup"
        return 1
    fi

    chmod 600 "$TOKEN_PATH"
    rm -f -- "$unit_backup"
    log "Web UI autostart ready: http://<server-ip>:${WEB_PORT}"
    log "Web token file: ${TOKEN_PATH}"
}

auto_rollback_directory() {
    local quarantine="$1"
    local backup_dir="$2"
    local service_was_active="$3"
    local backup_id failed_dir
    backup_id="$(basename "$backup_dir")"
    failed_dir="$(dirname "$INSTALL_DIR")/.syseba-failed-${backup_id}"

    warn "New version failed health checks. Restoring the previous directory automatically."
    "$SYSTEMCTL_BIN" stop "$SERVICE_NAME" >/dev/null 2>&1 || true
    if [[ -d "$INSTALL_DIR" ]]; then
        [[ ! -e "$failed_dir" ]] || failed_dir="${failed_dir}-$(date +%s)"
        mv -- "$INSTALL_DIR" "$failed_dir"
    fi
    mv -- "$quarantine" "$INSTALL_DIR"
    restore_unit_from_backup "$backup_dir"
    rm -f -- "$INSTALL_DIR"/*.lock 2>/dev/null || true

    if [[ "$service_was_active" -eq 1 ]]; then
        start_service || die "Automatic rollback restored files but ${SERVICE_NAME} still cannot start."
    fi
    warn "Old version restored. Failed new install retained at: $failed_dir"
}

cmd_backup() {
    local was_active=0 backup_dir
    check_backup_capacity
    if service_is_active; then
        was_active=1
        stop_service
        EXIT_RESTART_SERVICE=1
    else
        ensure_no_unmanaged_process
    fi

    backup_dir="$(create_backup manual "$was_active")"
    if [[ "$was_active" -eq 1 ]]; then
        start_service
        EXIT_RESTART_SERVICE=0
    fi
    printf 'Backup completed: %s\n' "$backup_dir"
}

cmd_update() {
    local ref="${1:-$DEFAULT_REF}"
    local force_start="${2:-0}"
    local was_active=0 should_start=0 backup_dir backup_id install_parent candidate quarantine
    validate_git_ref "$ref"
    [[ "$force_start" == "0" || "$force_start" == "1" ]] || die "Invalid update start policy."
    [[ -d "$INSTALL_DIR" ]] || die "Install directory not found: $INSTALL_DIR"
    [[ -f "$INSTALL_DIR/syseba.py" ]] || die "syseba.py not found in $INSTALL_DIR"

    install_parent="$(dirname "$INSTALL_DIR")"
    TEMP_STAGE="$(mktemp -d "${install_parent}/.syseba-stage.XXXXXX")"
    candidate="$TEMP_STAGE/repo"
    clone_release "$candidate" "$ref"
    log "Preflighting the downloaded release while SySeBa is still running..."
    verify_release_code "$candidate"
    check_backup_capacity

    if service_is_active; then
        was_active=1
        stop_service
        EXIT_RESTART_SERVICE=1
    else
        ensure_no_unmanaged_process
    fi

    backup_dir="$(create_backup pre-update "$was_active")"
    backup_id="$(basename "$backup_dir")"
    log "Preserving local configuration and runtime files..."
    copy_runtime_state "$candidate"
    verify_install "$candidate"

    quarantine="${install_parent}/.syseba-pre-update-${backup_id}"
    [[ ! -e "$quarantine" ]] || die "Quarantine path already exists: $quarantine"
    log "Switching ${INSTALL_DIR} to verified commit $(git_commit_for "$candidate")..."
    mv -- "$INSTALL_DIR" "$quarantine"
    if ! mv -- "$candidate" "$INSTALL_DIR"; then
        mv -- "$quarantine" "$INSTALL_DIR"
        die "Unable to move the staged release into place."
    fi
    rmdir "$TEMP_STAGE" 2>/dev/null || true
    TEMP_STAGE=""
    EXIT_RESTART_SERVICE=0

    chmod 750 "$INSTALL_DIR/syseba-maintenance.sh" 2>/dev/null || true
    rm -f -- "$INSTALL_DIR"/*.lock 2>/dev/null || true

    if [[ "$was_active" -eq 1 || "$force_start" -eq 1 ]]; then
        should_start=1
    fi
    if ! configure_web_service; then
        auto_rollback_directory "$quarantine" "$backup_dir" "$should_start"
        die "Update failed while enabling Web UI autostart; automatic rollback completed."
    fi
    if [[ "$should_start" -eq 1 ]]; then
        if ! start_service; then
            auto_rollback_directory "$quarantine" "$backup_dir" "$should_start"
            die "Update failed; automatic rollback completed."
        fi
    fi

    printf 'Update completed.\n'
    printf 'Installed commit: %s\n' "$(git_commit_for "$INSTALL_DIR")"
    printf 'Rollback snapshot: %s\n' "$backup_dir"
    printf 'Previous directory retained at: %s\n' "$quarantine"
}

cmd_quick_update() {
    local ref="${1:-$DEFAULT_REF}"
    local current target unit_before="" had_unit=0 was_active=0
    [[ -f "$INSTALL_DIR/syseba.py" ]] || die "syseba.py not found in $INSTALL_DIR"

    validate_git_ref "$ref"
    if ! target="$(resolve_target_commit "$ref")"; then
        die "Unable to resolve $ref from $REPO_URL. Check the ref and network connection."
    fi
    current="$(installed_identity)"
    printf 'Installed version: %s\n' "$current"
    printf 'Remote version:    %s (%s)\n' "$target" "$ref"

    if [[ "$current" == "$target" ]]; then
        log "The installed Git revision is already current; verifying it before restart."
        verify_install "$INSTALL_DIR"

        unit_before="$(mktemp)"
        if [[ -e "$UNIT_FILE" || -L "$UNIT_FILE" ]]; then
            cp -a -- "$UNIT_FILE" "$unit_before"
            had_unit=1
        fi
        if service_is_active; then
            was_active=1
            stop_service
            EXIT_RESTART_SERVICE=1
        else
            ensure_no_unmanaged_process
        fi

        if ! configure_web_service; then
            rm -f -- "$unit_before"
            die "The installed version is valid, but Web UI autostart configuration failed."
        fi
        if ! start_service; then
            warn "The Web-enabled unit failed to start; restoring the previous unit."
            "$SYSTEMCTL_BIN" stop "$SERVICE_NAME" >/dev/null 2>&1 || true
            if [[ "$had_unit" -eq 1 ]]; then
                cp -a -- "$unit_before" "$UNIT_FILE"
            else
                rm -f -- "$UNIT_FILE"
            fi
            "$SYSTEMCTL_BIN" daemon-reload
            EXIT_RESTART_SERVICE=0
            if [[ "$had_unit" -eq 1 && "$was_active" -eq 1 ]]; then
                start_service || die "Web migration failed and the original service could not be restarted."
            fi
            rm -f -- "$unit_before"
            die "Web migration failed; the previous systemd unit was restored."
        fi
        EXIT_RESTART_SERVICE=0
        rm -f -- "$unit_before"
        printf 'No update was required; no redundant snapshot was created.\n'
    else
        cmd_update "$target" 1
        service_is_active || die "The update completed but the service is not active."
    fi

    printf '\nService status: active\n'
    cmd_logs 40
    printf '\nLive view: %s follow\n' "$SCRIPT_NAME"
}

restore_previous_after_failed_rollback() {
    local current_quarantine="$1"
    local failed_restore="$2"
    local unit_before="$3"
    local current_was_active="$4"

    "$SYSTEMCTL_BIN" stop "$SERVICE_NAME" >/dev/null 2>&1 || true
    if [[ -d "$INSTALL_DIR" ]]; then
        mv -- "$INSTALL_DIR" "$failed_restore"
    fi
    mv -- "$current_quarantine" "$INSTALL_DIR"
    if [[ -f "$unit_before" ]]; then
        cp -a -- "$unit_before" "$UNIT_FILE"
    fi
    "$SYSTEMCTL_BIN" daemon-reload
    if [[ "$current_was_active" -eq 1 ]]; then
        start_service || true
    fi
}

select_backup_interactive() {
    [[ -t 0 ]] || die "Interactive rollback requires a terminal. Use rollback <backup-id> for non-interactive use."
    [[ -d "$BACKUP_ROOT" ]] || die "No backups found in $BACKUP_ROOT"

    local -a backup_ids=()
    local -a backup_dirs=()
    local directory id created reason commit version size index choice confirmation
    local selected_dir manifest archive_hash service_was_active

    while IFS= read -r directory; do
        [[ -f "$directory/manifest.txt" ]] || continue
        id="$(basename "$directory")"
        [[ "$id" =~ ^[0-9]{8}-[0-9]{6}(-[0-9]+)?$ ]] || continue
        backup_ids+=("$id")
        backup_dirs+=("$directory")
    done < <(find "$BACKUP_ROOT" -mindepth 1 -maxdepth 1 -type d -name '20*' -print | sort -r)

    (( ${#backup_ids[@]} > 0 )) || die "No valid backups found in $BACKUP_ROOT"

    printf '\nSySeBa rollback snapshots\n'
    printf 'Current install: %s\n' "$(installed_identity)"
    printf 'Snapshot root:   %s\n\n' "$BACKUP_ROOT"
    printf '%3s  %-18s %-19s %-11s %-12s %8s\n' "#" "BACKUP ID" "CREATED" "REASON" "VERSION" "SIZE"
    printf '%3s  %-18s %-19s %-11s %-12s %8s\n' "---" "------------------" "-------------------" "-----------" "------------" "--------"

    for index in "${!backup_ids[@]}"; do
        directory="${backup_dirs[$index]}"
        manifest="$directory/manifest.txt"
        created="$(manifest_value "$manifest" created_at)"
        reason="$(manifest_value "$manifest" reason)"
        commit="$(manifest_value "$manifest" git_commit)"
        if [[ "$commit" =~ ^[0-9a-fA-F]{40}$ ]]; then
            version="${commit:0:12}"
        else
            version="${commit:-legacy}"
        fi
        size="$(du -sh -- "$directory" 2>/dev/null | awk 'NR == 1 { print $1 }' || true)"
        printf '%3d  %-18.18s %-19.19s %-11.11s %-12.12s %8.8s\n' \
            "$((index + 1))" "${backup_ids[$index]}" "${created:-unknown}" \
            "${reason:-unknown}" "$version" "${size:-unknown}"
    done

    while true; do
        printf '\nSelect a snapshot [1-%d], or q to cancel: ' "${#backup_ids[@]}"
        if ! IFS= read -r choice; then
            printf '\n'
            return 1
        fi
        case "$choice" in
            q|Q) return 1 ;;
        esac
        if [[ "$choice" =~ ^[0-9]{1,4}$ ]]; then
            index=$((10#$choice - 1))
            if (( index >= 0 && index < ${#backup_ids[@]} )); then
                break
            fi
        fi
        warn "Invalid selection: $choice"
    done

    SELECTED_BACKUP="${backup_ids[$index]}"
    selected_dir="${backup_dirs[$index]}"
    manifest="$selected_dir/manifest.txt"
    created="$(manifest_value "$manifest" created_at)"
    reason="$(manifest_value "$manifest" reason)"
    commit="$(manifest_value "$manifest" git_commit)"
    service_was_active="$(manifest_value "$manifest" service_was_active)"
    archive_hash="$(awk '{ name = $2; sub(/^\*/, "", name); if (name == "syseba-app.tar.gz") { print $1; exit } }' "$selected_dir/SHA256SUMS" 2>/dev/null || true)"

    printf '\nSelected snapshot\n'
    printf '  Backup ID:       %s\n' "$SELECTED_BACKUP"
    printf '  Created:         %s\n' "${created:-unknown}"
    printf '  Reason:          %s\n' "${reason:-unknown}"
    printf '  Git commit:      %s\n' "${commit:-not-recorded}"
    printf '  Archive SHA-256: %s\n' "${archive_hash:-not-recorded}"
    printf '  Service active:  %s\n' "${service_was_active:-unknown}"
    printf '\nRestore this exact snapshot? [y/N]: '
    if ! IFS= read -r confirmation; then
        printf '\n'
        return 1
    fi
    case "$confirmation" in
        y|Y|yes|YES) return 0 ;;
        *) return 1 ;;
    esac
}

cmd_rollback_interactive() {
    SELECTED_BACKUP=""
    if ! select_backup_interactive; then
        printf 'Rollback canceled; no files were changed.\n'
        return 0
    fi
    acquire_maintenance_lock
    cmd_rollback "$SELECTED_BACKUP"
}

cmd_rollback() {
    local selector="${1:-latest}"
    local backup_dir backup_active current_was_active=0 install_parent timestamp
    local current_quarantine failed_restore unit_before should_start=0
    backup_dir="$(resolve_backup "$selector")"
    verify_backup "$backup_dir"
    [[ -d "$INSTALL_DIR" ]] || die "Install directory not found: $INSTALL_DIR"
    [[ -f "$INSTALL_DIR/syseba.py" ]] || die "syseba.py not found in $INSTALL_DIR"
    backup_active="$(manifest_value "$backup_dir/manifest.txt" service_was_active)"
    [[ "$backup_active" == "1" ]] || backup_active=0

    if service_is_active; then
        current_was_active=1
        stop_service
        EXIT_RESTART_SERVICE=1
    else
        ensure_no_unmanaged_process
    fi

    install_parent="$(dirname "$INSTALL_DIR")"
    timestamp="$(date +%Y%m%d-%H%M%S)"
    current_quarantine="${install_parent}/.syseba-before-rollback-${timestamp}"
    failed_restore="${install_parent}/.syseba-rollback-failed-${timestamp}"
    unit_before="${current_quarantine}.service-unit"
    [[ ! -e "$current_quarantine" ]] || die "Quarantine path already exists: $current_quarantine"

    if [[ -f "$UNIT_FILE" ]]; then
        cp -a -- "$UNIT_FILE" "$unit_before"
    fi
    mv -- "$INSTALL_DIR" "$current_quarantine"
    if ! "$TAR_BIN" -C / -xzf "$backup_dir/syseba-app.tar.gz"; then
        mv -- "$current_quarantine" "$INSTALL_DIR"
        die "Snapshot extraction failed; current install was restored."
    fi
    [[ -f "$INSTALL_DIR/syseba.py" ]] || {
        restore_previous_after_failed_rollback "$current_quarantine" "$failed_restore" "$unit_before" "$current_was_active"
        die "Rollback archive did not restore syseba.py; previous install restored."
    }

    restore_unit_from_backup "$backup_dir"
    rm -f -- "$INSTALL_DIR"/*.lock 2>/dev/null || true
    EXIT_RESTART_SERVICE=0
    if [[ "$current_was_active" -eq 1 || "$backup_active" -eq 1 ]]; then
        should_start=1
    fi

    if [[ "$should_start" -eq 1 ]] && ! start_service; then
        warn "Restored snapshot failed to start; returning to the pre-rollback install."
        restore_previous_after_failed_rollback "$current_quarantine" "$failed_restore" "$unit_before" "$current_was_active"
        die "Rollback failed; pre-rollback install restored. Failed snapshot retained at $failed_restore"
    fi

    printf 'Rollback completed from: %s\n' "$backup_dir"
    printf 'Replaced install retained at: %s\n' "$current_quarantine"
    if [[ -f "$backup_dir/external-state.tar.gz" ]]; then
        printf 'External state archive retained (not overwritten): %s\n' "$backup_dir/external-state.tar.gz"
    fi
}

cmd_list() {
    [[ -d "$BACKUP_ROOT" ]] || {
        printf 'No backups found in %s\n' "$BACKUP_ROOT"
        return 0
    }
    local directory id created reason commit
    printf '%-18s %-25s %-12s %s\n' "BACKUP ID" "CREATED" "REASON" "COMMIT"
    while IFS= read -r directory; do
        [[ -f "$directory/manifest.txt" ]] || continue
        id="$(basename "$directory")"
        created="$(manifest_value "$directory/manifest.txt" created_at)"
        reason="$(manifest_value "$directory/manifest.txt" reason)"
        commit="$(manifest_value "$directory/manifest.txt" git_commit)"
        printf '%-18s %-25s %-12s %s\n' "$id" "$created" "$reason" "$commit"
    done < <(find "$BACKUP_ROOT" -mindepth 1 -maxdepth 1 -type d -name '20*' -print | sort -r)
}

cmd_verify() {
    [[ -d "$INSTALL_DIR" ]] || die "Install directory not found: $INSTALL_DIR"
    verify_install "$INSTALL_DIR"
    printf 'Verification completed for %s\n' "$INSTALL_DIR"
}

cmd_logs() {
    local lines="${1:-100}"
    local log_path
    [[ "$lines" =~ ^[1-9][0-9]{0,3}$ ]] || die "Log line count must be between 1 and 2000."
    (( lines <= 2000 )) || die "Log line count must be between 1 and 2000."
    require_command journalctl
    require_command tail

    printf '\n=== SYSTEMD: %s (last %s lines) ===\n' "$SERVICE_NAME" "$lines"
    journalctl -u "$SERVICE_NAME" -n "$lines" --no-pager -o short-iso-precise || \
        warn "Unable to read the systemd journal."

    log_path="$(read_log_path || true)"
    if [[ -n "$log_path" && -f "$log_path" ]]; then
        printf '\n=== SYSEBA APPLICATION: %s (last %s lines) ===\n' "$log_path" "$lines"
        tail -n "$lines" -- "$log_path"
    elif [[ -n "$log_path" ]]; then
        printf '\n=== SYSEBA APPLICATION ===\n'
        warn "Configured log file does not exist yet: $log_path"
    else
        printf '\n=== SYSEBA APPLICATION ===\n'
        warn "Unable to determine the application log from $CONFIG_PATH"
    fi
}

cmd_follow() {
    local log_path
    require_command journalctl
    require_command tail
    log_path="$(read_log_path || true)"

    printf 'Following SySeBa logs. Press Ctrl+C to stop.\n'
    if [[ -n "$log_path" && -f "$log_path" ]]; then
        printf '\n=== SYSEBA APPLICATION: %s ===\n' "$log_path"
        tail -n 30 -F -- "$log_path" &
        FOLLOW_PID=$!
    elif [[ -n "$log_path" ]]; then
        warn "Configured log file does not exist yet: $log_path"
    else
        warn "Unable to determine the application log from $CONFIG_PATH"
    fi

    printf '\n=== SYSTEMD: %s ===\n' "$SERVICE_NAME"
    journalctl --follow -u "$SERVICE_NAME" -n 30 -o short-iso-precise
}

main() {
    local command="${1:-help}"
    shift || true

    case "$command" in
        help|-h|--help)
            usage
            return 0
            ;;
        quick-update|backup|update|rollback|list|verify|logs|follow)
            ;;
        *)
            usage >&2
            die "Unknown command: $command"
            ;;
    esac

    case "$command" in
        quick-update|update|rollback|logs)
            [[ "$#" -le 1 ]] || die "Too many arguments for $command"
            ;;
        *)
            [[ "$#" -eq 0 ]] || die "$command does not accept arguments"
            ;;
    esac
    validate_settings
    require_root
    resolve_tools

    case "$command" in
        quick-update|backup|update|rollback|list)
            validate_backup_location
            ;;
    esac

    case "$command" in
        quick-update|backup|update)
            acquire_maintenance_lock
            ;;
    esac

    case "$command" in
        quick-update) cmd_quick_update "${1:-$DEFAULT_REF}" ;;
        backup) cmd_backup ;;
        update) cmd_update "${1:-$DEFAULT_REF}" ;;
        rollback)
            if [[ "$#" -eq 0 ]]; then
                cmd_rollback_interactive
            else
                acquire_maintenance_lock
                cmd_rollback "$1"
            fi
            ;;
        list) cmd_list ;;
        verify) cmd_verify ;;
        logs) cmd_logs "${1:-100}" ;;
        follow) cmd_follow ;;
    esac
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
