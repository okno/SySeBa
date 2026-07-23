#!/usr/bin/env bash
set -euo pipefail

SYSEBA_BIN=${1:?path to syseba executable required}
ROOT=$(mktemp -d "${TMPDIR:-/tmp}/syseba-integration.XXXXXX")
PORT=$((18000 + ($$ % 1000)))
PID=

cleanup() {
  local status=$?
  if [[ -n "${PID}" ]] && kill -0 "${PID}" 2>/dev/null; then
    kill -TERM "${PID}" 2>/dev/null || true
    wait "${PID}" 2>/dev/null || true
  fi
  if [[ "${status}" -ne 0 && -s "${ROOT}/service.stderr" ]]; then
    printf '%s\n' '--- SySeBa stderr ---' >&2
    cat "${ROOT}/service.stderr" >&2
  fi
  rm -rf -- "${ROOT}"
}
trap cleanup EXIT INT TERM

mkdir -p "${ROOT}/source/docs" "${ROOT}/backup" "${ROOT}/restore"
printf 'alpha\n' >"${ROOT}/source/docs/alpha.txt"
ln -s /etc "${ROOT}/restore/escape"

cat >"${ROOT}/syseba.conf" <<EOF
[SETTINGS]
source = ${ROOT}/source
backup = ${ROOT}/backup
restore = ${ROOT}/restore
log = ${ROOT}/syseba.log
threads = 2
EOF
printf 'integration-token\n' >"${ROOT}/web.token"
chmod 600 "${ROOT}/web.token"

python3 - "${ROOT}/legacy.db" <<'PY'
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
connection.execute(
    """
    CREATE TABLE logs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp TEXT,
        operation TEXT,
        source_path TEXT,
        target_path TEXT,
        additional_info TEXT
    )
    """
)
connection.commit()
connection.close()
PY

wait_until() {
  local description=$1
  shift
  for _ in $(seq 1 100); do
    if "$@"; then
      return 0
    fi
    if [[ -n "${PID}" ]] && ! kill -0 "${PID}" 2>/dev/null; then
      printf 'daemon exited while waiting for: %s\n' "${description}" >&2
      return 1
    fi
    sleep 0.1
  done
  printf 'timeout: %s\n' "${description}" >&2
  return 1
}

"${SYSEBA_BIN}" run \
  --silent \
  --web \
  --web-host 127.0.0.1 \
  --web-port "${PORT}" \
  --config "${ROOT}/syseba.conf" \
  --lockfile "${ROOT}/syseba.lock" \
  --db-path "${ROOT}/legacy.db" \
  --web-token-file "${ROOT}/web.token" \
  >"${ROOT}/service.stdout" 2>"${ROOT}/service.stderr" &
PID=$!

wait_until "web API" \
  curl -fsS "http://127.0.0.1:${PORT}/api/auth" -o /dev/null
wait_until "initial copy" test -f "${ROOT}/backup/docs/alpha.txt"
cmp "${ROOT}/source/docs/alpha.txt" "${ROOT}/backup/docs/alpha.txt"

unauthorized=$(curl -sS -o /dev/null -w '%{http_code}' \
  "http://127.0.0.1:${PORT}/api/status")
[[ "${unauthorized}" == "401" ]]

curl -fsS \
  -H 'X-SySeBa-Token: integration-token' \
  "http://127.0.0.1:${PORT}/api/status" |
  grep -q '"version":"2.0.0"'

printf 'beta\n' >"${ROOT}/source/docs/alpha.txt"
wait_until "modified copy" \
  grep -q beta "${ROOT}/backup/docs/alpha.txt"

printf 'zeta\n' >"${ROOT}/source/docs/alpha.txt"
touch -d '@946684800' "${ROOT}/source/docs/alpha.txt"
wait_until "same-size file with older timestamp" \
  grep -q zeta "${ROOT}/backup/docs/alpha.txt"

if "${SYSEBA_BIN}" run \
  --silent \
  --no-initial-sync \
  --config "${ROOT}/syseba.conf" \
  --lockfile "${ROOT}/syseba.lock" \
  --db-path "${ROOT}/contender.db" \
  >"${ROOT}/contender.stdout" 2>"${ROOT}/contender.stderr"; then
  printf 'a second daemon acquired the active lock\n' >&2
  exit 1
fi
grep -q 'already running' "${ROOT}/contender.stderr"
running_status=$(
  "${SYSEBA_BIN}" status \
    --json \
    --config "${ROOT}/syseba.conf" \
    --lockfile "${ROOT}/syseba.lock"
)
grep -Eq '"running":[[:space:]]*true' <<<"${running_status}"

rm "${ROOT}/source/docs/alpha.txt"
wait_until "soft delete" test -f "${ROOT}/restore/docs/alpha.txt"
test ! -e "${ROOT}/backup/docs/alpha.txt"

"${SYSEBA_BIN}" restore-copy \
  --config "${ROOT}/syseba.conf" \
  --db-path "${ROOT}/legacy.db" \
  --lockfile "${ROOT}/maintenance.lock" \
  --path docs/alpha.txt >/dev/null
wait_until "CLI restore" test -f "${ROOT}/source/docs/alpha.txt"
grep -q zeta "${ROOT}/source/docs/alpha.txt"

printf 'web-restore\n' >"${ROOT}/source/docs/web.txt"
wait_until "web backup" test -f "${ROOT}/backup/docs/web.txt"
rm "${ROOT}/source/docs/web.txt"
wait_until "web soft delete" test -f "${ROOT}/restore/docs/web.txt"

curl -fsS \
  -H 'X-SySeBa-Token: integration-token' \
  -H 'Content-Type: application/json' \
  -d '{"path":"docs/web.txt","strategy":"fail"}' \
  "http://127.0.0.1:${PORT}/api/restore" |
  grep -q '"ok":true'
wait_until "Web restore" test -f "${ROOT}/source/docs/web.txt"

curl -fsS \
  -H 'X-SySeBa-Token: integration-token' \
  "http://127.0.0.1:${PORT}/restore/download?path=docs/web.txt" |
  grep -q web-restore

invalid_path=$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'X-SySeBa-Token: integration-token' \
  "http://127.0.0.1:${PORT}/api/restore/info?path=../../etc/passwd")
[[ "${invalid_path}" == "400" ]]

symlink_escape=$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'X-SySeBa-Token: integration-token' \
  "http://127.0.0.1:${PORT}/restore/download?path=escape/passwd")
[[ "${symlink_escape}" == "404" ]]

curl -fsS \
  -H 'X-SySeBa-Token: integration-token' \
  -H 'Content-Type: application/json' \
  -d '{"source":"source"}' \
  "http://127.0.0.1:${PORT}/api/config" |
  grep -Fq "\"source\":\"${ROOT}/source\""
grep -Fq "source = ${ROOT}/source" "${ROOT}/syseba.conf"

fractional_threads=$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'X-SySeBa-Token: integration-token' \
  -H 'Content-Type: application/json' \
  -d '{"threads":2.5}' \
  "http://127.0.0.1:${PORT}/api/config")
[[ "${fractional_threads}" == "400" ]]

config_injection=$(curl -sS -o /dev/null -w '%{http_code}' \
  -H 'X-SySeBa-Token: integration-token' \
  -H 'Content-Type: application/json' \
  -d "{\"source\":\"${ROOT}/source\\nthreads = 64\"}" \
  "http://127.0.0.1:${PORT}/api/config")
[[ "${config_injection}" == "400" ]]
grep -Fq 'threads = 2' "${ROOT}/syseba.conf"

python3 - "${ROOT}/legacy.db" <<'PY'
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
columns = {
    row[1] for row in connection.execute("PRAGMA table_info(logs)").fetchall()
}
assert "level" in columns, columns
assert connection.execute("SELECT COUNT(*) FROM logs").fetchone()[0] > 0
connection.close()
PY

kill -TERM "${PID}"
wait "${PID}"
PID=

test -f "${ROOT}/syseba.lock"
status_output=$(
  "${SYSEBA_BIN}" status \
    --json \
    --config "${ROOT}/syseba.conf" \
    --lockfile "${ROOT}/syseba.lock" || true
)
grep -Eq '"running":[[:space:]]*false' <<<"${status_output}"

set +e
short_token_output=$(
  "${SYSEBA_BIN}" run \
    --silent \
    --web-only \
    --web-host 127.0.0.1 \
    --web-port "$((PORT + 1))" \
    --web-token short \
    --config "${ROOT}/syseba.conf" \
    --lockfile "${ROOT}/short.lock" \
    --db-path "${ROOT}/short.db" 2>&1
)
short_token_status=$?
set -e
[[ "${short_token_status}" -ne 0 ]]
grep -q '16-255' <<<"${short_token_output}"

set +e
no_auth_output=$(
  "${SYSEBA_BIN}" run \
    --silent \
    --web-only \
    --no-web-auth \
    --web-host 0.0.0.0 \
    --web-port "$((PORT + 2))" \
    --config "${ROOT}/syseba.conf" \
    --lockfile "${ROOT}/no-auth.lock" \
    --db-path "${ROOT}/no-auth.db" 2>&1
)
no_auth_status=$?
set -e
[[ "${no_auth_status}" -ne 0 ]]
grep -q 'loopback' <<<"${no_auth_output}"

ln -s /etc/passwd "${ROOT}/bad-web.token"
set +e
symlink_token_output=$(
  "${SYSEBA_BIN}" run \
    --silent \
    --web-only \
    --web-host 127.0.0.1 \
    --web-port "$((PORT + 3))" \
    --config "${ROOT}/syseba.conf" \
    --lockfile "${ROOT}/symlink-token.lock" \
    --db-path "${ROOT}/symlink-token.db" \
    --web-token-file "${ROOT}/bad-web.token" 2>&1
)
symlink_token_status=$?
set -e
[[ "${symlink_token_status}" -ne 0 ]]
grep -q 'Unable to read/create web token' <<<"${symlink_token_output}"

test -z "$(find "${ROOT}/backup" "${ROOT}/source" \
  -name '*.syseba-tmp.*' -print -quit)"
grep -q '\[INFO\]' "${ROOT}/syseba.log"
test ! -s "${ROOT}/service.stderr"
printf 'linux integration: OK\n'
