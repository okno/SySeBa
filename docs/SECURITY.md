# Security Model

[Italiano](SECURITY.it.md) | [Documentation index](README.md)

## Scope

This document covers filesystem input, local configuration/state, the built-in
HTTP service, service privileges, third-party code, maintenance archives, and
release artifacts.

## Assets

- Availability and integrity of source data.
- Integrity and completeness of the current backup.
- Confidentiality and integrity of restore content.
- Web token, configuration, audit database, and logs.
- Exact rollback capability for the installed software.

## Trust Boundaries

SySeBa trusts the operating-system kernel and the administrator who selects
roots and installs the service. File names and directory contents inside
configured trees are untrusted. HTTP clients are untrusted until their token
is validated. Git/network release input is untrusted until built and tested;
current maintenance snapshots are protected by local SHA-256 manifests, not
by an external signature.

Published binaries are available through GitHub Releases and a public GHCR
OCI mirror. The OCI mirror is a transport container, not a service runtime.
Its digest authenticates the registry object fetched from GitHub; the
enclosed `SHA256SUMS` verifies the individual release payloads.

## Filesystem Defenses

- Root paths are normalized and checked for overlap.
- Restore input must be relative and contained under both restore and source.
- Sensitive reads reject non-regular final objects.
- POSIX uses `O_NOFOLLOW`; Windows opens reparse points explicitly and rejects
  them.
- Destination temporary files use exclusive creation and unique names.
- A destination symlink is replaced, never followed.
- Source identity and metadata are checked after copying.
- Final replacement is atomic on the destination filesystem.
- Config and token saves use temporary file, flush, and atomic replacement.

Intermediate component replacement by an administrator remains outside the
threat model. Runtime directories should be owned by root/SYSTEM and not
writable by untrusted users.

## Process Control

The lock is a held kernel object, not a PID-file convention. PID reuse cannot
make `status` report a false owner. The on-disk lock file deliberately
persists so release cannot race a second process acquiring a newly created
inode.

Signal handlers only set a pending flag. This avoids async-signal-unsafe heap,
mutex, logger, and Web operations.

## HTTP Authentication

- Generated token: 32 cryptographic random bytes encoded as 64 hex digits.
- Accepted explicit token: 16-255 printable ASCII characters.
- Token can come from CLI, environment, or protected file.
- `--no-web-auth` is rejected for non-loopback bind addresses.
- Comparison is performed server-side for every protected route.
- JSON bodies are limited to 64 KiB.
- Restore pages are capped at 250 entries; log output at 2,000 lines.
- Download names are sanitized and sent as octet-stream with `nosniff`.

The server intentionally does not implement TLS. Exposure requirements:

1. trusted LAN/VPN only, with host firewall restrictions; or
2. authenticated TLS reverse proxy terminating HTTPS on a separate port.

Do not forward port 8765 from an Internet router.

The token is a bearer credential. Browser storage and reverse-proxy logs must
not expose it. Prefer the `X-SySeBa-Token` header; never place it in a URL.

## Service Sandboxing

The generated Linux unit uses:

```text
NoNewPrivileges=true
CapabilityBoundingSet=CAP_DAC_OVERRIDE CAP_FOWNER
PrivateTmp=true
PrivateDevices=true
ProtectSystem=full
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
```

`ReadWritePaths` is generated from source, backup, restore, and runtime-state
parents. Source must be writable because restore is an explicit feature.

Windows uses LocalSystem, a service SID, and protected token DACL. macOS uses
a root LaunchDaemon. These broad privileges are operationally necessary for
arbitrary data trees; deployments with homogeneous ownership may substitute a
dedicated service account after ACL and restore testing.

## SQLite

The database path must be regular before open. SQL statements are fixed or
prepared; path and message values are bound parameters. Extension loading and
double-quoted string compatibility are disabled at compile time.

SQLite pathname open still assumes its parent directory is administrator
controlled. Do not place the database under a world-writable directory.

## Maintenance and Supply Chain

- Git refs are syntax-validated before use.
- Candidate code builds outside the active installation.
- Tests and `config-check` run before the service is stopped.
- Snapshots are taken only with the service stopped.
- Archives include ACL/xattr and numeric ownership.
- Every snapshot has `SHA256SUMS` and identity metadata.
- Rollback checks hashes and rejects absolute/parent-traversal members.
- Failed post-install health checks restore the prior application and unit.
- The GHCR workflow uses repository-scoped `contents: read` and
  `packages: write` permissions.
- It downloads an existing tagged Release instead of rebuilding unreviewed
  branch content.
- It validates the version syntax, exact expected filenames, non-empty files,
  and every published SHA-256 before pushing.
- The only external workflow action is pinned to a full commit SHA.
- The final OCI carrier uses `scratch`, so it adds no package-manager or shell
  runtime.

SHA-256 detects local corruption but does not authenticate a malicious local
administrator. The current 2.0.0 Git tags are annotated but not
cryptographically signed. Windows artifacts lack Authenticode, and the macOS
DMG lacks Developer ID signing/notarization. Future production releases
should add those controls and detached release signatures.

## Compiler Hardening

Release builds enable warnings, stack protector, `_FORTIFY_SOURCE`, RELRO,
immediate binding, NX/ASLR/high-entropy VA where supported, and Windows
Control Flow Guard under MSVC. `MemoryDenyWriteExecute` adds runtime W^X on
systemd.

## Security Test Cases

Automated integration covers traversal, restore symlink escape, token symlink
rejection, short-token rejection, remote unauthenticated bind rejection,
duplicate lock acquisition, atomic destination replacement, and old SQLite
schema migration. ASan and UBSan runs cover the shared Linux code.

## Residual Risks

- No built-in TLS or request rate limiter.
- Polling may detect a macOS change later than a native event backend.
- A privileged actor can change roots or replace intermediate components.
- Source mutation faster than retry policy may leave an error requiring the
  next event/reconciliation.
- Published Windows/macOS artifacts are unsigned and can produce platform
  trust warnings.
- SySeBa does not make data immutable or protect against physical media loss.

Report suspected vulnerabilities privately before publishing exploit details.
