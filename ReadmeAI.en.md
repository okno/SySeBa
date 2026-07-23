# SySeBa - Complete Operations Guide

![SySeBa logo](SySeBa_Logo.webp)

[Quick English README](README.md) | [README italiano](README.it.md) | [Guida avanzata in italiano](ReadmeAI.md)

This guide covers installation, configuration, runtime behavior, Web UI,
console, security, updates, rollback, logs, troubleshooting, and restore
operations. Commands assume Linux with systemd and an installation at
`/opt/syseba`.

## 1. What SySeBa does

SySeBa continuously maintains a copy of a source directory:

```text
source  --copy and update-->  backup
source  --deletion--------->  backup -> restore
restore --recovery--------->  source
```

It performs a multithreaded initial scan at startup, then uses `watchdog` and
Linux inotify to process creations, modifications, moves, and deletions in
real time.

When an item is deleted from the source, its backup copy is moved to the
restore area instead of being permanently deleted. SySeBa chooses a unique
name when needed, so an older retained copy is not overwritten accidentally.

SySeBa does not replace offline, immutable, or remote backups. Source, backup,
and restore storage should remain part of a broader disaster-recovery plan.

## 2. Components and paths

| File or path | Purpose |
|---|---|
| `/opt/syseba/syseba.py` | Engine, CLI, console dashboard, and Web server |
| `/opt/syseba/syseba_web.js` | Web UI behavior and translations |
| `/opt/syseba/syseba.lang` | IT/EN console and CLI labels |
| `/opt/syseba/syseba.conf` | Active configuration |
| `/opt/syseba/syseba_logs.db` | SQLite audit database |
| `/opt/syseba/syseba_web.token` | Persistent Web token, mode `0600` |
| `/opt/syseba/syseba.lock` | Active-instance lock |
| `/var/log/syseba.log` | Default application log |
| `/etc/systemd/system/syseba.service` | Generated systemd unit |
| `/root/syseba-backups` | Recommended maintenance snapshot location |

Database files, WAL/SHM sidecars, logs, locks, tokens, and snapshots are
excluded from Git.

## 3. Requirements

- Linux with systemd.
- Python 3.8 or newer.
- Python packages `watchdog` and `psutil`.
- Git, GNU tar, coreutils, `flock`, `journalctl`, and `sha256sum` for the
  automated maintenance workflow.
- Read access to the source and write access to backup, restore, log, and
  application directories.

Base packages on Debian or Ubuntu:

```bash
sudo apt update
sudo apt install -y git python3 python3-pip python3-venv
```

## 4. Fresh installation

Clone the release:

```bash
sudo git clone https://github.com/okno/SySeBa.git /opt/syseba
sudo python3 -m pip install -r /opt/syseba/requirements.txt
sudo chmod 750 /opt/syseba/syseba.py /opt/syseba/syseba-maintenance.sh
```

If the distribution blocks global `pip` installs, use equivalent
distribution packages or configure an explicitly managed Python environment.
The default unit uses `/usr/bin/python3`.

Prepare directories:

```bash
sudo mkdir -p /storage/data /storage/backup /storage/restore
sudo touch /var/log/syseba.log
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

Validate before startup:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --lang en
```

Install and start the service:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --lang en
sudo systemctl start syseba.service
```

Verify:

```bash
systemctl is-enabled syseba.service
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
```

## 5. Configuration

The `[SETTINGS]` section is required.

| Key | Meaning | Rules |
|---|---|---|
| `source` | Original monitored tree | Must exist and be readable |
| `backup` | Current synchronized copy | Must be writable |
| `restore` | Retained deleted items | Must be writable |
| `log` | Text log file | Its parent directory must be writable |
| `threads` | Initial-sync workers | Positive integer |

Paths may be absolute or relative to the configuration file directory. They
must not overlap. For example, the backup must not be inside the source and
the restore tree must not be inside the backup.

More threads do not automatically mean better performance. `2-5` workers are
usually appropriate for mechanical disks; SSDs or fast storage pools may
benefit from more. Measure actual latency, I/O, and load.

Human-readable validation:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --lang en
```

Automation-friendly JSON:

```bash
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf \
  --json
```

Web UI changes are validated before being written. If paths or worker count
change, the page displays `Restart required`; the watcher continues with its
active configuration until the service is restarted.

## 6. Service and automatic Web startup

`service-install` always generates and enables a unit that starts the watcher
and Web UI together:

```text
/usr/bin/python3 /opt/syseba/syseba.py
  --silent
  --web
  --web-host 0.0.0.0
  --web-port 8765
  --lang en
  --config /opt/syseba/syseba.conf
  --web-token-file /opt/syseba/syseba_web.token
```

The real `ExecStart` is a single line. Binding to `0.0.0.0` makes the page
available through the server's LAN interfaces. The unit is enabled for
`multi-user.target` and automatically restarts after failures or host reboots.

To replace an old unit that contains only `--silent`:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --lang en
sudo systemctl restart syseba.service
```

Alternatively, `syseba-maintenance.sh quick-update` migrates and validates the
unit even when the installed commit is already current.

Custom address or port:

```bash
sudo python3 /opt/syseba/syseba.py service-install \
  --config /opt/syseba/syseba.conf \
  --web-host 192.168.1.10 \
  --web-port 9876 \
  --lang en
sudo systemctl restart syseba.service
```

## 7. Web UI access

Open:

```text
http://SERVER_IP:8765
```

Read the token:

```bash
sudo cat /opt/syseba/syseba_web.token
sudo stat -c '%a %U:%G %n' /opt/syseba/syseba_web.token
```

The token is generated once with cryptographic entropy, written atomically,
protected with mode `0600`, and reused across restarts. SySeBa rejects a token
path that is a symbolic link or not a regular file.

The browser stores the token in `sessionStorage`. It remains within the tab
session and can be removed with `Forget token`.

The Web UI provides:

- service state, PID, uptime, initial-sync progress, and lock state;
- disk usage, CPU, memory, thread, queue, and session counters;
- logs filtered by text and severity;
- active-versus-saved configuration comparison;
- validated configuration editing and service restart;
- restore browsing with breadcrumbs, search, sorting, and pagination;
- item details, file download, and file or directory recovery;
- safe-rename or explicit-overwrite conflict handling.

The built-in HTTP server does not provide TLS. Restrict the port to the
required LAN:

```bash
sudo ufw allow from 192.168.1.0/24 to any port 8765 proto tcp
```

Do not publish port `8765` directly to the Internet. Use a VPN or an
authenticated HTTPS reverse proxy for remote access.

## 8. Manual startup and operating modes

Watcher with console dashboard:

```bash
sudo python3 /opt/syseba/syseba.py \
  --config /opt/syseba/syseba.conf \
  --lang en
```

Manual watcher and Web UI, bound to localhost by default:

```bash
sudo python3 /opt/syseba/syseba.py \
  --web \
  --config /opt/syseba/syseba.conf \
  --lang en
```

Web administration without a watcher:

```bash
sudo python3 /opt/syseba/syseba.py \
  --web-only \
  --config /opt/syseba/syseba.conf \
  --lang en
```

Service-style process without the interactive dashboard:

```bash
sudo python3 /opt/syseba/syseba.py \
  --silent \
  --web \
  --config /opt/syseba/syseba.conf \
  --lang en
```

Use `--no-initial-sync` only after evaluating the risk of leaving the backup
out of sync. Use `--no-web-auth` only in isolated tests.

The `/opt/syseba/syseba.lock` file prevents two watchers from using one
installation. `--web-only` does not acquire the watcher lock.

## 9. Console dashboard

The console dashboard adapts to terminal width and height:

- full layout on wide terminals;
- compact layout when vertical space is limited;
- stable bars for source, backup, restore, CPU, and initial sync;
- status, uptime, queue, workers, counters, and recent events;
- no color escape sequences when standard output is not a TTY or `NO_COLOR`
  is set.

Do not start another dashboard while `syseba.service` is active. Inspect state
without a second watcher:

```bash
sudo python3 /opt/syseba/syseba.py status \
  --config /opt/syseba/syseba.conf \
  --lang en
```

## 10. Languages

Supported languages:

```bash
--lang it
--lang en
```

The choice controls the dashboard, CLI commands, operational messages, and Web
UI. The generated service keeps the language in `ExecStart`.

`syseba.lang` uses:

```text
KEY;Testo italiano;English text
```

Web translations live in `syseba_web.js`. After editing translations, ensure
that every key exists in both languages and run the tests.

## 11. CLI reference

| Command | Purpose |
|---|---|
| `run` | Start the watcher; this is the default command |
| `status` | Show state, lock, PID, paths, and disks |
| `logs --lines N` | Read the latest application log lines |
| `config-check` | Validate configuration and paths |
| `restore-list` | List and search the restore area |
| `restore-copy --path PATH` | Recover an item to the source |
| `service-install` | Generate and enable the Web-enabled unit |

Common options:

| Option | Purpose |
|---|---|
| `--config PATH` | Alternate configuration |
| `--lang it\|en` | Language |
| `--json` | JSON output |
| `--silent` | Disable the interactive dashboard |
| `--web` | Start Web UI with the watcher |
| `--web-only` | Web administration only |
| `--web-host HOST` | Listen address |
| `--web-port PORT` | Port from `1` to `65535` |
| `--web-token TOKEN` | Direct token; avoid in production shells |
| `--web-token-file PATH` | Persistent token file |
| `--no-web-auth` | Disable Web authentication |
| `--no-initial-sync` | Skip the initial scan |

Examples:

```bash
sudo python3 /opt/syseba/syseba.py status --json
sudo python3 /opt/syseba/syseba.py logs --lines 200
sudo python3 /opt/syseba/syseba.py restore-list --search report --page-size 100
sudo python3 /opt/syseba/syseba.py restore-copy \
  --path documents/report.pdf \
  --rename
```

Without `--rename` or `--overwrite`, an existing destination stops the
restore. `--rename` selects an unused path; `--overwrite` explicitly replaces
a file or merges a directory.

## 12. HTTP API

The `/` page, static assets, and `/api/auth` are public so the sign-in form can
load without repeated failed requests. `/api/auth` only reports whether a
token is required. Every operational API and download requires:

```http
X-SySeBa-Token: TOKEN
```

`Authorization: Bearer TOKEN` is also accepted.

| Method | Endpoint | Purpose |
|---|---|---|
| `GET` | `/api/auth` | Report whether authentication is required |
| `GET` | `/api/status` | Complete status |
| `GET` | `/api/logs?lines=200` | Recent log lines |
| `GET` | `/api/config` | Saved configuration |
| `GET` | `/api/config/state` | Active/saved comparison |
| `POST` | `/api/config` | Validate and save configuration |
| `GET` | `/api/restore` | Paginated restore listing |
| `GET` | `/api/restore/info?path=...` | Item details and conflicts |
| `POST` | `/api/restore` | Restore using a selected strategy |
| `GET` | `/restore/download?path=...` | Download a file |
| `POST` | `/api/service/restart` | Request a systemd restart |

Example:

```bash
TOKEN="$(sudo cat /opt/syseba/syseba_web.token)"
curl -sS \
  -H "X-SySeBa-Token: ${TOKEN}" \
  http://127.0.0.1:8765/api/status
```

JSON request size is limited. Restore paths are normalized and checked against
directory traversal and symbolic-link escapes.

## 13. Logs and audit

Recommended combined view:

```bash
cd /root
/root/SySeBa-release/syseba-maintenance.sh logs 200
```

Follow the systemd and application streams together:

```bash
/root/SySeBa-release/syseba-maintenance.sh follow
```

Direct commands:

```bash
journalctl -fu syseba.service -o short-iso-precise
tail -n 200 -F /var/log/syseba.log
```

The journal is the primary source for startup, shutdown, Python exceptions,
and restart policy. The configured log records application activity and file
operations. SQLite stores structured audit events with timestamp, severity,
action, path, details, and outcome.

At startup, `initialize_database()` inspects the schema and adds columns
missing from older versions. This prevents the repeating
`table logs has no column named level` failure without deleting history.

## 14. Safe automated updates

Keep a separate maintenance checkout:

```bash
cd /root
git clone https://github.com/okno/SySeBa.git SySeBa-release
cd /root
sudo /root/SySeBa-release/syseba-maintenance.sh quick-update
```

Do not run `git pull` directly inside `/opt/syseba` while the service is
running.

`quick-update`:

1. compares the installed identity with the remote commit;
2. downloads to staging and checks syntax and dependencies;
3. verifies available snapshot space;
4. stops the service and rejects unmanaged processes;
5. creates a consistent checksummed snapshot;
6. preserves configuration, DB, WAL/SHM, log, and token;
7. validates the candidate configuration;
8. swaps the application directory;
9. regenerates and validates automatic Web startup;
10. starts the service and checks that it stays active;
11. prints recent logs and the rollback location.

If the commit is already current, no redundant snapshot is created. The tool
still verifies the installation, migrates an old service unit, and restarts
the service.

Available commands:

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh quick-update
sudo /root/SySeBa-release/syseba-maintenance.sh backup
sudo /root/SySeBa-release/syseba-maintenance.sh update main
sudo /root/SySeBa-release/syseba-maintenance.sh verify
sudo /root/SySeBa-release/syseba-maintenance.sh list
sudo /root/SySeBa-release/syseba-maintenance.sh logs 200
sudo /root/SySeBa-release/syseba-maintenance.sh follow
```

The default snapshot root is relative to the launch directory:

```text
./syseba-backups/YYYYMMDD-HHMMSS/
```

Launching from `/root` therefore produces
`/root/syseba-backups/...`.

Main overrides:

| Variable | Default |
|---|---|
| `SYSEBA_INSTALL_DIR` | `/opt/syseba` |
| `SYSEBA_BACKUP_ROOT` | `./syseba-backups` |
| `SYSEBA_REPO_URL` | Official repository |
| `SYSEBA_REF` | `main` |
| `SYSEBA_CONFIG_PATH` | `/opt/syseba/syseba.conf` |
| `SYSEBA_DB_PATH` | `/opt/syseba/syseba_logs.db` |
| `SYSEBA_TOKEN_PATH` | `/opt/syseba/syseba_web.token` |
| `SYSEBA_WEB_HOST` | `0.0.0.0` |
| `SYSEBA_WEB_PORT` | `8765` |
| `SYSEBA_LANG` | `it` |
| `SYSEBA_HEALTH_WAIT` | `3` seconds |

Example using another port:

```bash
sudo env SYSEBA_WEB_PORT=9876 SYSEBA_LANG=en \
  /root/SySeBa-release/syseba-maintenance.sh quick-update
```

## 15. Snapshot contents

Each snapshot contains:

```text
manifest.txt
SHA256SUMS
syseba-app.tar.gz
syseba.service
external-state.tar.gz       when needed
external-state.paths        when needed
```

`syseba-app.tar.gz` contains the full `/opt/syseba` installation except lock
files and Python caches. Configuration, token, DB, or log files outside the
installation are stored in `external-state.tar.gz`.

The maintenance tool never copies, deletes, or replaces the configured
`source`, `backup`, and `restore` data trees.

## 16. Rollback

Text menu:

```bash
cd /root
sudo /root/SySeBa-release/syseba-maintenance.sh rollback
```

The selector shows ID, date, reason, commit, size, original service state, and
archive hash. Nothing changes before confirmation.

Direct selection:

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh rollback pre-update
sudo /root/SySeBa-release/syseba-maintenance.sh rollback latest
sudo /root/SySeBa-release/syseba-maintenance.sh rollback 20260723-023230
```

All SHA-256 checksums are verified first. The current installation moves to
quarantine, the original unit is restored, and the restored service undergoes
a health check.

If the snapshot fails to start, the tool automatically restores the
installation that was active before the rollback attempt. The failed release
remains available for diagnostics.

Rollback restores the exact application and state stored inside
`/opt/syseba`, including configuration, database, and token when they lived
there. External state is archived but not overwritten automatically, to avoid
replacing external files changed since the snapshot.

## 17. Security

- Use Web token authentication; do not pass the token on a production command
  line.
- Keep `/opt/syseba/syseba_web.token` at mode `0600`.
- Restrict `8765/tcp` to the required LAN or VPN.
- Do not expose the built-in HTTP server directly to the Internet.
- Do not use `--no-web-auth` on shared networks.
- Protect `/root/syseba-backups`; it contains configuration and runtime state.
- Keep offline or remote copies of important snapshots.
- Test restore and rollback periodically, not only snapshot creation.
- Keep Python and its dependencies current through system maintenance.

The generated unit applies `NoNewPrivileges`, `PrivateTmp`, `ProtectSystem`,
`ProtectHome`, umask `0077`, and an explicit writable-path list. If custom
destinations are denied by systemd, inspect the journal and carefully adapt
`ReadWritePaths`.

## 18. Troubleshooting

### The Web UI does not respond

```bash
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service | grep ExecStart
ss -lntp | grep ':8765'
journalctl -u syseba.service -b -n 100 --no-pager
```

`ExecStart` must contain `--web`, host, port, and token file. If it contains
only `--silent`, run `quick-update` or regenerate the unit with
`service-install`, then restart it.

### The service listens only on localhost

The unit must include:

```text
--web-host 0.0.0.0
```

Then run:

```bash
sudo systemctl daemon-reload
sudo systemctl restart syseba.service
```

### The token is rejected

```bash
sudo stat -c '%a %U:%G %n' /opt/syseba/syseba_web.token
sudo cat /opt/syseba/syseba_web.token
```

Expected mode is `600`. Click `Forget token` in the browser and enter the
current value. Check for extra spaces or lines.

### The service enters a restart loop

```bash
journalctl -u syseba.service -b -n 200 --no-pager
sudo python3 /opt/syseba/syseba.py config-check \
  --config /opt/syseba/syseba.conf
```

Check dependencies, permissions, free space, available mounts, and path
overlap.

### SQLite reports `no column named level`

The current release migrates the schema before starting the writer:

```bash
sudo systemctl restart syseba.service
journalctl -u syseba.service -n 100 --no-pager
```

Do not delete the database first. If the error remains, preserve the DB, WAL,
and SHM files, stop the service, and confirm it really runs the updated
`/opt/syseba/syseba.py`.

### Saved configuration is not active

```bash
sudo systemctl restart syseba.service
```

This distinction is intentional: it prevents the watcher from changing its
root while handling events.

### The lock reports another instance

```bash
systemctl status syseba.service
cat /opt/syseba/syseba.lock
ps -fp "$(cat /opt/syseba/syseba.lock)"
```

Do not remove the lock while its PID is active. If no SySeBa process exists, a
stale lock can be removed while the service is stopped.

### Restore is rejected

Confirm that the requested path is relative to the restore area, does not
escape through an external symlink, and that the source is writable. For an
existing destination, explicitly select rename or overwrite.

### The updater stops

Read its final error, then run:

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh list
sudo /root/SySeBa-release/syseba-maintenance.sh verify
sudo /root/SySeBa-release/syseba-maintenance.sh logs 200
```

If failure occurs after the directory swap, the tool attempts automatic
rollback. Keep `.syseba-*` directories until diagnosis is complete.

## 19. FAQ

### Which port does the Web UI use?

The default port is `8765`. With the standard service, open
`http://SERVER_IP:8765`.

### Does the Web UI start automatically at boot?

Yes. The unit generated by `service-install` contains `--web`, is enabled for
`multi-user.target`, and uses `Restart=always`.

### Can I use SySeBa only from the console?

Yes. A manual start without `--web` displays the dashboard. The standard
service still includes the Web UI for local remote administration.

### Can I inspect the Web UI without starting the watcher?

Yes, with `--web-only`. This is suitable for inspection and management, but
does not synchronize files.

### Does an update modify my data trees?

The maintenance tool does not copy or replace `source`, `backup`, or
`restore`. Events occurring during the update stop are reconciled by the
initial sync at startup.

### Does rollback really restore the previous version?

Yes, when the snapshot and checksums are valid. It restores the full archived
installation and systemd unit. The replaced version stays quarantined, and if
the restored service does not start, the tool automatically puts it back.

### Can I change the token?

Yes. Stop the service, retain a copy of the current token, replace the file
with a long random value, apply `chmod 600`, and restart. Do not use a symbolic
link.

### Why is the token absent from Git?

It is a local credential. Publishing it would let anyone who knows it read
logs, change configuration, and request restore operations.

### Does the SQLite log grow over time?

Yes. Plan monitoring and retention according to disk and audit requirements.
Perform SQLite maintenance only while the service is stopped and after a
snapshot.

## 20. Pre-production verification

```bash
sudo /root/SySeBa-release/syseba-maintenance.sh verify
sudo systemctl restart syseba.service
sudo systemctl is-active syseba.service
ss -lntp | grep ':8765'
sudo /root/SySeBa-release/syseba-maintenance.sh logs 100
```

Also test:

- Web sign-in from an authorized LAN host;
- test-file creation and modification;
- deletion into restore;
- restore with free and conflicting destinations;
- manual snapshot and rollback selection;
- firewall rules;
- free space on source, backup, restore, and snapshot storage.

## 21. Developer tests

```bash
python3 -m unittest discover -s tests -v
python3 -m py_compile syseba.py
bash -n syseba-maintenance.sh
```

The suite covers SQLite migration, locking, copy and restore behavior, path
security, protected APIs, persistent tokens, systemd units, console layout,
localization, and CLI output.

## 22. License and project

SySeBa is released under the MIT license. See [LICENSE](LICENSE).

Repository: [okno/SySeBa](https://github.com/okno/SySeBa)
