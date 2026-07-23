# SySeBa - Syncro Service Backup

![SySeBa logo](SySeBa_Logo.webp)

[English](README.md) | [Italiano](README.it.md) | [Advanced operations guide](ReadmeAI.en.md)

SySeBa is a Linux continuous file-protection daemon. It performs an initial
multithreaded synchronization, mirrors subsequent filesystem changes in real
time, and preserves deleted backup copies in a separate restore area instead
of permanently removing them.

The system includes an adaptive console dashboard, a token-protected Web UI,
structured text and SQLite logs, safe restore workflows, and an automated
update/rollback tool.

## Main features

- Initial multithreaded copy from source to backup.
- Real-time monitoring through `watchdog` and Linux inotify.
- Soft deletion: files removed from source are moved from backup to restore.
- Copy retries for transient write and rename races.
- Text logging plus SQLite audit records with automatic schema migration.
- Responsive console dashboard for narrow and short terminals.
- Italian and English CLI, console, Web UI, operational messages, and docs.
- Web status, process metrics, disk usage, searchable logs, configuration
  editing, restart control, and restore browser.
- Restore search, sorting, pagination, download, safe rename, and explicit
  overwrite/merge.
- Single-instance process lock.
- Hardened systemd service with Web UI enabled at boot.
- Persistent Web token created with mode `0600`.
- Staged updates, stopped-state snapshots, health checks, and automatic
  rollback.
- Interactive text selector for restoring an exact software snapshot.

## Architecture

| Path | Purpose |
|---|---|
| `/opt/syseba` | Application, configuration, runtime database, token, lock |
| `/var/log/syseba.log` | Default text log |
| `source` | Authoritative data monitored by SySeBa |
| `backup` | Current synchronized copy |
| `restore` | Versioned soft-deletion area |
| `/etc/systemd/system/syseba.service` | Generated service unit |
| `/root/syseba-backups` | Recommended maintenance snapshots |

SySeBa never treats the restore area as disposable storage. A source deletion
moves the corresponding backup item into restore under a unique name when
required.

## Requirements

- Linux with systemd.
- Python 3.8 or newer.
- `watchdog`.
- `psutil`.
- Git, GNU tar, coreutils, and `flock` for the maintenance tool.

On Debian or Ubuntu:

```bash
sudo apt update
sudo apt install -y git python3 python3-pip python3-venv
```

## Fresh installation

```bash
sudo git clone https://github.com/okno/SySeBa.git /opt/syseba
sudo python3 -m pip install -r /opt/syseba/requirements.txt
sudo chmod 750 /opt/syseba/syseba.py /opt/syseba/syseba-maintenance.sh
```

Edit `/opt/syseba/syseba.conf`:

```ini
[SETTINGS]
source = /storage/data
backup = /storage/backup
restore = /storage/restore
log = /var/log/syseba.log
threads = 5
```

Validate the configuration:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --lang en
```

Install the service:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --lang en
sudo systemctl start syseba.service
```

`service-install` always enables the Web UI in the generated systemd unit:

```text
--silent
--web
--web-host 0.0.0.0
--web-port 8765
--web-token-file /opt/syseba/syseba_web.token
```

The token is generated once, stored with mode `0600`, and reused after every
restart.

## Web UI

Open:

```text
http://SERVER_IP:8765
```

Read the authentication token on the server:

```bash
sudo cat /opt/syseba/syseba_web.token
```

Check that the service is listening:

```bash
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
```

The systemd service listens on all network interfaces because it is intended
for a trusted local network. Restrict port `8765` to the required LAN subnet
with the host firewall. Do not expose the built-in HTTP server directly to the
Internet; use a VPN or an authenticated TLS reverse proxy.

Manual Web runs remain local-only by default:

```bash
sudo python3 /opt/syseba/syseba.py \
  --web \
  --config /opt/syseba/syseba.conf
```

The command above binds to `127.0.0.1`. Use `--web-host 0.0.0.0` only when LAN
access is intentional.

## Console and language

Italian console:

```bash
sudo python3 /opt/syseba/syseba.py \
  --config /opt/syseba/syseba.conf \
  --lang it
```

English console:

```bash
sudo python3 /opt/syseba/syseba.py \
  --config /opt/syseba/syseba.conf \
  --lang en
```

`syseba.lang` contains the complete editable IT/EN console and CLI label set in
this format:

```text
KEY;Italian text;English text
```

Web translations are shipped in `syseba_web.js`. The selected `--lang` is
carried into the systemd service and controls console, CLI, Web UI, and
operational log messages.

## CLI reference

| Command | Description |
|---|---|
| `run` | Start watcher and console; default command |
| `status` | Show lock state, PID, paths, and disk usage |
| `logs --lines N` | Print recent application log lines |
| `config-check` | Validate paths, overlaps, thread count, and log placement |
| `restore-list` | Search and browse the restore area |
| `restore-copy --path PATH` | Restore one file or directory |
| `service-install` | Generate and enable the Web-enabled systemd unit |

Important options:

| Option | Description |
|---|---|
| `--config PATH` | Alternate configuration file |
| `--lang it\|en` | Interface and operational-message language |
| `--silent` | Disable the interactive console dashboard |
| `--web` | Start Web UI with the watcher |
| `--web-only` | Start Web UI without the filesystem watcher |
| `--web-host HOST` | Listen address |
| `--web-port PORT` | Listen port, default `8765` |
| `--web-token-file PATH` | Persistent authentication token |
| `--no-web-auth` | Disable authentication; unsafe outside isolated tests |
| `--no-initial-sync` | Skip the startup full scan |
| `--json` | Machine-readable output for operational commands |

Restore examples:

```bash
sudo python3 /opt/syseba/syseba.py restore-list \
  --config /opt/syseba/syseba.conf \
  --search report \
  --page 1 \
  --page-size 100

sudo python3 /opt/syseba/syseba.py restore-copy \
  --config /opt/syseba/syseba.conf \
  --path documents/report.pdf \
  --rename
```

Use `--overwrite` only when replacing or merging the destination is intended.

## Safe update

Clone a separate maintenance checkout. Do not clone over a working
`/opt/syseba` installation.

```bash
cd /root
git clone https://github.com/okno/SySeBa.git SySeBa-release
cd /root
/root/SySeBa-release/syseba-maintenance.sh quick-update
```

The updater:

1. Resolves and compares local and remote revisions.
2. Downloads and syntax-checks the release while SySeBa is still running.
3. Checks free backup space.
4. Stops the service.
5. Creates a checksummed stopped-state snapshot.
6. Preserves configuration, database, WAL/SHM files, logs, and Web token.
7. Validates the staged configuration.
8. Atomically switches application directories.
9. Regenerates and validates Web UI systemd autostart.
10. Starts the service and checks that it remains active.
11. Automatically restores the previous version if validation or startup
    fails.

When launched from `/root`, snapshots are stored under:

```text
/root/syseba-backups/YYYYMMDD-HHMMSS/
```

The updater does not copy or alter the configured source, backup, or restore
data trees.

## Rollback

Interactive text selector:

```bash
cd /root
/root/SySeBa-release/syseba-maintenance.sh rollback
```

The selector shows backup ID, creation time, reason, Git commit, size, and the
archive SHA-256 before confirmation.

Direct rollback:

```bash
/root/SySeBa-release/syseba-maintenance.sh rollback pre-update
/root/SySeBa-release/syseba-maintenance.sh rollback latest
/root/SySeBa-release/syseba-maintenance.sh rollback 20260723-023230
```

Before extraction, checksums are verified. The replaced installation is kept
in quarantine. If the restored service cannot start, SySeBa puts the
pre-rollback installation back.

## Logs

Application and systemd history:

```bash
/root/SySeBa-release/syseba-maintenance.sh logs 200
```

Follow both sources:

```bash
/root/SySeBa-release/syseba-maintenance.sh follow
```

Direct commands:

```bash
journalctl -fu syseba.service -o short-iso-precise
tail -n 200 -F /var/log/syseba.log
```

Service lifecycle and Python errors appear in the journal. File operations are
written primarily to the configured text log and SQLite database.

## Security model

- Web API mutations require `X-SySeBa-Token`.
- The Web token file is never tracked by Git and is mode `0600`.
- Token-file symbolic links are rejected.
- JSON request bodies are size-limited.
- Restore paths are contained with canonical-path and symlink checks.
- The process lock prevents duplicate watchers.
- The systemd unit uses `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`, a
  restrictive umask, and explicit writable paths.
- Runtime databases, WAL files, logs, locks, tokens, and maintenance snapshots
  are excluded from Git.
- Maintenance snapshots have SHA-256 manifests.

## Troubleshooting

### Web UI is not reachable

```bash
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
journalctl -u syseba.service -b -n 100 --no-pager
```

The unit must contain `--web --web-host 0.0.0.0 --web-port 8765`.

### Invalid Web token

```bash
sudo stat -c '%a %U:%G %n' /opt/syseba/syseba_web.token
sudo cat /opt/syseba/syseba_web.token
```

Expected mode is `600`. Clear the browser session token and enter the current
file value again.

### SQLite reports a missing `level` column

Restart the current release. `initialize_database()` migrates older schemas
before the log writer starts.

### Configuration changed but paths did not

Saving configuration does not silently switch active watcher paths. Restart:

```bash
sudo systemctl restart syseba.service
```

### SySeBa is already running

Do not start a second manual watcher while the service is active:

```bash
systemctl status syseba.service
cat /opt/syseba/syseba.lock
```

## Tests

```bash
python3 -m unittest discover -s tests -v
```

The suite covers schema migration, lock handling, path traversal and symlink
defenses, protected Web APIs, persistent token files, generated systemd units,
restore workflows, console layout, localization, and CLI JSON output.

## License

MIT. See [LICENSE](LICENSE).

Project maintained by [okno](https://github.com/okno).
