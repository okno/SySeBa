# SySeBa 2 - Complete Manual

![SySeBa logo](SySeBa_Logo.webp)

[English README](README.md) | [README italiano](README.it.md) |
[Manuale completo italiano](ReadmeAI.md)

This manual covers the native C11 release of SySeBa: architecture,
installation, configuration, CLI, dashboard, Web UI, services, logging,
restore semantics, security, tuning, migration, rollback, and diagnostics.

## Published Versions and Distribution

| Line | Status | Source | Downloads |
|---|---|---|---|
| SySeBa 2.0.0 native C | Current | `main`; tag `v2.0.0` for released binaries | [Release](https://github.com/okno/SySeBa/releases/tag/v2.0.0) |
| SySeBa 1.0.0 Python | Legacy compatibility and rollback | `legacy/python`; tag `v1.0.0-python` | [Legacy release](https://github.com/okno/SySeBa/releases/tag/v1.0.0-python) |

Ready-to-install 2.0.0 artifacts are available in GitHub Releases and are also
mirrored as the public static OCI package
[`ghcr.io/okno/syseba-packages`](https://github.com/okno/SySeBa/pkgs/container/syseba-packages).
The OCI package is not the SySeBa service itself: it carries all nine release
files below `/packages`.

```bash
docker pull ghcr.io/okno/syseba-packages:2.0.0
container=$(docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true)
docker cp "$container:/packages" ./syseba-packages
docker rm "$container"
cd syseba-packages
sha256sum -c SHA256SUMS
```

The `2.0.0` and `latest` package tags currently resolve to
`sha256:823bfa56d87f2ed3deb817c4483cfe4e5951139e4820bae4a69473f0790173f8`.
The repository workflow downloads the tagged Release, validates every
expected filename and checksum, then publishes with a scoped
`GITHUB_TOKEN`.

## 1. Purpose and Boundaries

SySeBa maintains three directory trees:

```text
source  --initial sync/events--> backup
source  --deletion------------> backup -> restore
restore --controlled recovery-> source
```

`source` is authoritative, `backup` is its current synchronized copy, and
`restore` preserves items removed from the source. Unless
`--no-initial-sync` is used, startup recursively scans the source and queues
every missing or changed file before entering continuous watch mode.

SySeBa complements rather than replaces immutable snapshots, offline copies,
remote versioning, and disaster recovery. A compromised administrator or
simultaneous storage failure can still affect all configured trees.

## 2. Version 2 Changes

- Entire C11 runtime; no Python, `watchdog`, `psutil`, or `pip`.
- One executable for engine, CLI, console, Web UI, and service management.
- inotify and `ReadDirectoryChangesW`, with a portable polling fallback.
- Exclusive temporary files, source stability checks, durable flush, and
  atomic destination replacement.
- Kernel-enforced single-instance locking.
- Random 256-bit Web token and protected token storage.
- Transactional migration of historical SQLite schemas.
- Bilingual responsive console and Web UI.
- Native Linux, Windows, and macOS packages.

## 3. Default Paths

| State | Linux | Windows | macOS |
|---|---|---|---|
| Config | `/etc/syseba/syseba.conf` | `C:\ProgramData\SySeBa\syseba.conf` | `/usr/local/etc/syseba/syseba.conf` |
| Database | `/var/lib/syseba/syseba_logs.db` | `C:\ProgramData\SySeBa\syseba_logs.db` | `/usr/local/var/lib/syseba/syseba_logs.db` |
| Token | `/etc/syseba/syseba_web.token` | `C:\ProgramData\SySeBa\syseba_web.token` | `/usr/local/etc/syseba/syseba_web.token` |
| Lock | `/run/syseba/syseba.lock` | `C:\ProgramData\SySeBa\syseba.lock` | `/usr/local/var/run/syseba/syseba.lock` |
| Text log | `/var/log/syseba/syseba.log` | `C:\ProgramData\SySeBa\syseba.log` | `/usr/local/var/log/syseba/syseba.log` |

Linux and macOS also recognize legacy state under `/opt/syseba` during
migration.

## 4. Configuration

The historical INI format remains compatible:

```ini
[SETTINGS]
source = /srv/syseba/source
backup = /srv/syseba/backup
restore = /srv/syseba/restore
log = /var/log/syseba/syseba.log
threads = 4
```

`threads` must be in the range 1-64. Source, backup, and restore must be
distinct, non-nested roots. The log must not live under a monitored tree.
The source must exist; runtime state parents and target trees must be
creatable and writable.

Validate before starting:

```bash
syseba config-check --config /etc/syseba/syseba.conf --lang en
syseba config-check --config /etc/syseba/syseba.conf --json
```

A configuration saved through the Web UI does not silently replace the paths
used by the running process. Restart the service to apply it.

## 5. Linux Installation

Debian package:

```bash
sudo apt install ./syseba_2.0.0_amd64.deb
sudoedit /etc/syseba/syseba.conf
sudo syseba config-check --lang en
sudo systemctl enable --now syseba.service
```

RPM package:

```bash
sudo rpm -Uvh syseba-2.0.0-1.x86_64.rpm
sudoedit /etc/syseba/syseba.conf
sudo syseba config-check --lang en
sudo systemctl enable --now syseba.service
```

Source build:

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DSYSEBA_BUILD_TESTS=ON \
  -DSYSEBA_ENABLE_HARDENING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo syseba service-install --config /etc/syseba/syseba.conf --lang en
```

SQLite, cJSON, and CivetWeb are vendored and linked into the executable.

## 6. Windows 11 and Windows Server

Extract the portable ZIP or run the NSIS installer, then open an elevated
PowerShell prompt:

```powershell
.\install-service.ps1
notepad C:\ProgramData\SySeBa\syseba.conf
Restart-Service SySeBa
Get-Service SySeBa
Get-Content C:\ProgramData\SySeBa\syseba.log -Wait -Tail 100
```

The service uses automatic startup and LocalSystem. The token file receives a
protected DACL for SYSTEM, Administrators, and the owner.

## 7. macOS

The DMG contains an unsigned Universal 2 executable for Intel and Apple
Silicon:

```bash
cd /Volumes/SySeBa
sudo ./install.sh
```

The published 2.0.0 DMG is not code-signed or notarized. Production
distribution should add Developer ID signing, Apple notarization, stapling,
and execution tests on real Intel and Apple Silicon hardware.

```bash
sudo launchctl print system/com.okno.syseba
log stream --predicate 'process == "syseba"' --style compact
tail -F /usr/local/var/log/syseba/syseba.log
```

## 8. CLI Reference

Commands:

```text
run               start initial sync, watcher, and console
status            show lock, process, and disk state
logs              print the text-log tail
config-check      validate configuration
restore-list      list/search restore items
restore-copy      restore a selected item
restore-browser   interactive text restore selector
service-install   install and enable the native service
```

Main options:

| Option | Meaning |
|---|---|
| `--config PATH` | Alternate configuration |
| `--lang it\|en` | CLI, console, and Web language |
| `--silent` | Disable ANSI dashboard |
| `--web` | Run Web UI with watcher |
| `--web-only` | Web UI without watcher |
| `--web-host ADDRESS` | Manual bind, default `127.0.0.1` |
| `--web-port PORT` | Port, default `8765` |
| `--web-token TOKEN` | Explicit token |
| `--web-token-file PATH` | Persistent token |
| `--no-web-auth` | Allowed only on loopback |
| `--no-initial-sync` | Skip startup scan |
| `--lockfile PATH` | Alternate process lock |
| `--db-path PATH` | Alternate SQLite database |
| `--console-refresh SEC` | Dashboard interval |
| `--json` | Machine-readable output |

Restore options are `--path`, `--search`, `--page`, `--page-size`,
`--sort name|mtime|size`, `--direction asc|desc`, `--rename`, and
`--overwrite`.

```bash
syseba status --json
syseba logs --lines 500
syseba restore-list --search invoice --sort mtime --direction desc
syseba restore-copy --path customers/invoice.pdf --rename
syseba restore-browser
```

## 9. Console Dashboard

Interactive `run` renders a stable adaptive screen with service/watcher/Web
state, initial-sync progress, disk bars, CPU and memory, operation counters,
queue depth, paths, and recent activity. It switches to compact layouts for
narrow or short terminals.

Services use `--silent`; lifecycle output remains available through the OS
service log and file operations remain in the configured application log.

## 10. Web UI

The installed service enables the Web UI at:

```text
http://SERVER_IP:8765
```

Retrieve the Linux token:

```bash
sudo cat /etc/syseba/syseba_web.token
```

The root page and `/api/auth` are public so the login view can load. Every
data API requires:

```http
X-SySeBa-Token: <token>
```

The UI provides health and process metrics, disk usage, recent events, text
logs, saved/active configuration comparison, validated edits, restore search
and pagination, item details/download, three restore strategies, and
controlled service restart.

The embedded server is HTTP only. Restrict it to a trusted LAN/VPN or place it
behind an authenticated TLS reverse proxy. Do not expose it directly to the
Internet.

## 11. Copy and Restore Semantics

Each file copy creates an exclusive sibling temporary file, streams data
without following the final destination link, verifies that source identity,
size, and modification time remained stable, flushes the data, and atomically
replaces the destination. Transient changes are retried.

Deletion moves the current backup item into restore. Existing restore names
receive `.YYYYMMDD-HHMMSS` and a counter if needed.

Restore strategies:

- `fail`: refuse an existing destination.
- `rename`: create `name.restored-YYYYMMDD-HHMMSS.ext`.
- `overwrite`: replace a file or merge a compatible directory.

Absolute paths, `..`, and symlink traversal outside configured roots are
rejected.

## 12. Logging and SQLite Migration

SySeBa writes a text log, a SQLite audit table, and the native service log.
SQLite uses WAL, `synchronous=NORMAL`, a five-second busy timeout, and a
dedicated writer thread.

At startup a `BEGIN IMMEDIATE` migration creates `logs` or individually adds
missing `timestamp`, `level`, `operation`, `source_path`, `target_path`, and
`additional_info` columns. This directly repairs old databases that produced:

```text
sqlite3.OperationalError: table logs has no column named level
```

Linux live logs:

```bash
sudo journalctl -fu syseba.service -o short-iso-precise
sudo tail -n 200 -F /var/log/syseba/syseba.log
sudo ./scripts/syseba-maintenance.sh follow
```

Never remove SQLite `-wal` or `-shm` files while the process is active.

## 13. Migration, Updates, and Rollback

Use a checkout separate from `/opt/syseba`:

```bash
cd /root
git clone https://github.com/okno/SySeBa.git SySeBa-release
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/migrate-from-python.sh
```

The tool builds and tests while the old daemon runs, stops the service,
creates a checksummed stopped-state snapshot, preserves config, DB/WAL/SHM,
token, log, and unit, atomically switches the application directory, starts
the hardened Web-enabled service, and performs a health check. A failed
post-switch check automatically restores the exact previous installation.

Interactive and direct rollback:

```bash
sudo ./scripts/syseba-maintenance.sh rollback
sudo ./scripts/syseba-maintenance.sh rollback pre-update
sudo ./scripts/syseba-maintenance.sh rollback 20260723-023230
```

The software snapshot deliberately excludes configured source, backup, and
restore trees. Therefore it restores application state exactly but cannot
undo subsequent changes to data volumes.

Future update:

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/syseba-maintenance.sh quick-update main
```

## 14. Security Summary

- No scripting runtime in production.
- 256-bit generated tokens; explicit tokens are validated.
- Authentication cannot be disabled on non-loopback addresses.
- 64 KiB JSON limit and bounded restore pages.
- Sensitive final path components reject symlinks/reparse points.
- Atomic config/token writes and kernel process locking.
- Canonical containment checks for every restore path.
- Hardened systemd sandbox and explicit writable paths.
- Checksummed maintenance archives with member validation.

SySeBa normally runs as root/SYSTEM because it must read and restore arbitrary
trees. A dedicated account is possible when every storage ACL and state path
is prepared accordingly.

See [docs/SECURITY.md](docs/SECURITY.md) for the threat model.

## 15. Performance Tuning

`threads` controls filesystem workers only; watcher and log writer use
separate threads.

| Workload | Suggested starting point |
|---|---|
| Single HDD | 2-4 |
| RAID/NAS | 4-8 |
| SSD/NVMe | 4-16 |
| Very large files | Keep concurrency conservative |
| Millions of tiny files | Raise gradually while watching queue depth |

Excess workers can increase seeks, metadata contention, cache pressure, and
network traffic. Measure rather than assuming a larger value is faster.

## 16. Troubleshooting

Web UI:

```bash
systemctl status syseba.service --no-pager -l
systemctl cat syseba.service
ss -lntp | grep ':8765'
journalctl -u syseba.service -b -n 150 --no-pager
```

Duplicate instance:

```bash
syseba status
systemctl status syseba.service
```

The `.lock` file persists by design; only the kernel lock proves ownership.

Configuration:

```bash
syseba config-check --json
sudo systemctl restart syseba.service
```

Diagnostic set:

```bash
syseba --version
syseba status --json
syseba config-check --json
systemctl status syseba.service --no-pager -l
journalctl -u syseba.service -b -n 300 --no-pager
```

Remove tokens and sensitive paths before sharing diagnostics.

## 17. Build and Test

All artifacts from Linux/WSL:

```bash
./scripts/build-release.sh
```

Windows entry point:

```powershell
.\scripts\build-release.ps1
```

Developer verification:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --parallel
ctest --test-dir build --output-on-failure
shellcheck scripts/*.sh tests_c/*.sh packaging/**/*.sh
```

The release pipeline checks native tests, Linux integration, maintenance
rollback behavior, Windows execution, both macOS architectures, package
contents, Linux glibc compatibility, and SHA-256 manifests.

The local release scripts intentionally stop after creating and validating
`dist/syseba-2.0.0`. Publication is a separate controlled operation:

- GitHub Releases contains the directly downloadable installers and archives.
- `.github/workflows/publish-packages.yml` mirrors a selected release to
  GitHub Packages after repeating filename and checksum validation.
- The OCI image is `scratch`-based and contains no operating-system runtime.
- Windows executables are currently unsigned; the macOS DMG is unsigned and
  not notarized.

Further documentation:

- [Bilingual documentation index](docs/README.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Build](docs/BUILD.md)
- [Packaging](docs/PACKAGING.md)
- [Testing](docs/TESTING.md)
- [HTTP API](docs/API.md)
- [Operations](docs/OPERATIONS.md)
- [Migration](docs/MIGRATION.md)
- [Security](docs/SECURITY.md)

## 18. Licenses

SySeBa is MIT licensed. SQLite is public domain, cJSON is MIT, and CivetWeb
is MIT. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
