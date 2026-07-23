# SySeBa 2

![SySeBa logo](SySeBa_Logo.webp)

[Italiano](README.it.md) | [Complete English guide](ReadmeAI.en.md) |
[Guida completa italiana](ReadmeAI.md)

SySeBa is a native C11 continuous backup service for Windows, Linux, and
macOS. It performs a parallel initial synchronization, watches the source
tree for changes, maintains the current copy in `backup`, and moves deleted
items into a browsable `restore` area.

Version 2 has no Python runtime dependency. One executable provides the
filesystem engine, bilingual console dashboard, CLI, SQLite audit log,
token-protected Web UI, restore browser, and native service integration.

## Release Lines

- **SySeBa 2.x (native C)** is the current line on `main`, with packages for
  Linux, Windows, and macOS.
- **SySeBa 1.x (Python Legacy)** remains available from the
  [`legacy/python`](https://github.com/okno/SySeBa/tree/legacy/python) branch
  and the
  [GitHub Releases](https://github.com/okno/SySeBa/releases/tag/v1.0.0-python)
  page for compatibility and exact rollback.

Both implementations remain downloadable. New platform support and feature
development target the native C line; the Python line is retained for
existing installations and critical compatibility or security corrections.

## Highlights

- Native watchers: inotify on Linux, `ReadDirectoryChangesW` on Windows, and
  a portable polling fallback.
- Bounded worker model with a dedicated serialized log writer.
- Atomic file replacement, source stability checks, retry handling, and
  single-instance kernel locking.
- Automatic migration of every historical SQLite `logs` schema, including
  databases that do not yet contain the `level` column.
- Italian and English CLI, console dashboard, Web UI, messages, and guides.
- Web status, process and disk metrics, live logs, validated configuration
  editing, service restart, restore search, preview, download, and recovery.
- Hardened systemd unit, Windows Service, and macOS LaunchDaemon support.
- Consistent stopped-state snapshots, exact-version rollback, checksums, and
  automatic rollback after a failed upgrade.

## Release Artifacts

Download published versions from
[GitHub Releases](https://github.com/okno/SySeBa/releases). The local release
builder produces:

| Platform | Artifact |
|---|---|
| Linux x86_64 | Portable `.tar.gz` executable bundle |
| Debian/Ubuntu x86_64 | `.deb` |
| RPM Linux x86_64 | `.rpm` |
| Windows 11/Server x86_64 | Portable `.zip` and NSIS setup `.exe` |
| macOS 11+ Intel and Apple Silicon | Universal 2 `.dmg` |
| Source | Reproducible source `.tar.gz` with vendored dependencies |

Run `scripts/build-release.sh` under Linux/WSL, or
`scripts/build-release.ps1` from Windows. Artifacts are written to `dist/`;
the scripts never publish or push anything.

### GitHub Packages

The same verified artifacts are mirrored in the
[`ghcr.io/okno/syseba-packages`](https://github.com/users/okno/packages/container/package/syseba-packages)
OCI package. It is a static artifact carrier, not a runtime container:

```bash
docker pull ghcr.io/okno/syseba-packages:2.0.0
container=$(docker create ghcr.io/okno/syseba-packages:2.0.0 /bin/true)
docker cp "$container:/packages" ./syseba-packages
docker rm "$container"
```

Validate the extracted files with `sha256sum -c syseba-packages/SHA256SUMS`.

## Linux Install

Package install:

```bash
sudo apt install ./syseba_2.0.0_amd64.deb
sudoedit /etc/syseba/syseba.conf
sudo systemctl enable --now syseba.service
```

Source build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
sudo cmake --install build
sudo syseba service-install --config /etc/syseba/syseba.conf --lang en
```

Default Linux configuration:

```ini
[SETTINGS]
source = /srv/syseba/source
backup = /srv/syseba/backup
restore = /srv/syseba/restore
log = /var/log/syseba/syseba.log
threads = 4
```

## Upgrade From the Python Release

Do not overwrite `/opt/syseba` manually. From a separate checkout:

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/migrate-from-python.sh
```

The migration tool builds and tests the C candidate while the old daemon is
still running, stops the service, creates a checksummed snapshot in the
current directory, preserves configuration/database/log/token state, switches
the installation atomically, starts the Web-enabled service, and performs a
health check. A failed health check restores the previous installation
automatically.

Exact rollback:

```bash
sudo ./scripts/syseba-maintenance.sh rollback
sudo ./scripts/syseba-maintenance.sh rollback pre-update
```

The interactive command lists the available software snapshots before asking
for confirmation. Source, backup, and restore data trees are never included
in or changed by software snapshots.

## Web UI and Logs

The installed service starts the Web UI automatically on port `8765`:

```text
http://SERVER_IP:8765
```

Linux token and logs:

```bash
sudo cat /etc/syseba/syseba_web.token
sudo journalctl -fu syseba.service -o short-iso-precise
sudo tail -n 200 -F /var/log/syseba/syseba.log
```

The built-in server is HTTP. Restrict port `8765` to a trusted LAN/VPN or put
it behind an authenticated TLS reverse proxy. It is not intended for direct
Internet exposure.

## CLI

```text
syseba run
syseba status [--json]
syseba logs --lines 200
syseba config-check
syseba restore-list [--search TEXT] [--json]
syseba restore-copy --path RELATIVE [--rename|--overwrite]
syseba restore-browser
syseba service-install
```

Run `syseba --help --lang en` for every option.

## Documentation

- [Complete English guide](ReadmeAI.en.md)
- [Guida completa italiana](ReadmeAI.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Build and release engineering](docs/BUILD.md)
- [Security model](docs/SECURITY.md)
- [Operations and observability](docs/OPERATIONS.md)
- [Migration and rollback](docs/MIGRATION.md)
- [HTTP API](docs/API.md)
- [Testing](docs/TESTING.md)
- [Packaging](docs/PACKAGING.md)

## License

SySeBa is released under the MIT License. See [LICENSE](LICENSE) and
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
