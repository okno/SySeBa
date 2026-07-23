# Migration and Rollback

[Italiano](MIGRATION.it.md) | [Documentation index](README.md)

## Objective

Migrate an active Python-era installation in `/opt/syseba` to the C runtime
without losing the exact pre-upgrade application state and without modifying
configured source, backup, or restore trees.

## Preconditions

- Linux systemd host.
- Existing service name `syseba.service`.
- Root privileges.
- Separate release checkout, normally `/root/SySeBa-release`.
- C compiler, CMake, Git, tar, SHA-256 tools, and enough local snapshot space.
- Existing configuration passes the new root-overlap rules.

Do not clone into `/opt/syseba`; that directory is the rollback subject.

Use `main` for the native C line. The former Python implementation remains
available at tag `v1.0.0-python` and branch `legacy/python`; do not switch the
maintenance checkout to that branch when performing a C migration.

## Automated Migration

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/migrate-from-python.sh
```

`migrate-from-python.sh` delegates to:

```bash
sudo ./scripts/syseba-maintenance.sh install-local
```

## Transaction Boundary

Pre-stop work:

1. Validate maintenance paths and acquire a maintenance lock.
2. Configure/compile candidate in a sibling temporary directory.
3. Run native and available integration tests.
4. Run candidate `config-check` against the current configuration.

Stopped-state work:

1. Record prior service active/enabled state.
2. Stop and wait for systemd inactivity.
3. Create installation archive and external-state archive.
4. Hash archives and manifest.
5. Copy state into the staged C installation.
6. Rename active installation to a sibling previous directory.
7. Rename stage to the active installation path.
8. Generate hardened systemd unit and start service.
9. Require active service plus successful local `/api/auth`.

Failure after step 6 calls automatic rollback. A failed candidate is retained
in a sibling quarantine directory for diagnostics.

## Snapshot Contents

Included:

- entire install directory except lock files;
- external config, DB, WAL, SHM, Web token, and application log when outside
  the install directory;
- active systemd unit;
- ACLs, extended attributes, numeric owners;
- creation reason, service state, paths, software identity, and checksums.

Excluded:

- configured source tree;
- configured backup tree;
- configured restore tree.

This keeps software rollback bounded and prevents a maintenance operation from
copying or replacing user data volumes.

## Rollback Selection

```bash
sudo ./scripts/syseba-maintenance.sh list
sudo ./scripts/syseba-maintenance.sh rollback
```

Direct selectors:

```bash
sudo ./scripts/syseba-maintenance.sh rollback latest
sudo ./scripts/syseba-maintenance.sh rollback pre-update
sudo ./scripts/syseba-maintenance.sh rollback YYYYMMDD-HHMMSS
```

Before extraction, SySeBa validates the SHA-256 manifest, install-directory
identity, and every archive member. Absolute and parent-traversal entries are
rejected.

## Rollback Guarantee

`pre-update` restores the stopped-state application, config, SQLite files,
token, text log, ownership/ACL metadata, and systemd unit captured immediately
before switching. It does not roll data trees back in time. If the new version
processed source events, those data effects require storage snapshots or
normal restore operations.

## Future Git Update

```bash
cd /root/SySeBa-release
git status --short
git pull --ff-only origin main
sudo ./scripts/syseba-maintenance.sh quick-update main
```

The first three commands update only the maintenance checkout. `quick-update`
compares remote commit to installed `BUILD-INFO`, then runs the same
transaction if needed.

## Recovery if systemd Cannot Run

Do not improvise archive extraction over a running daemon. First inspect:

```bash
sudo ./scripts/syseba-maintenance.sh list
sudo sha256sum -c /root/syseba-backups/ID/SHA256SUMS
sudo systemctl stop syseba.service
```

Prefer the scripted rollback because it preserves the failed installation and
reverts a failed rollback attempt. Manual extraction is a last-resort
administrator procedure.
