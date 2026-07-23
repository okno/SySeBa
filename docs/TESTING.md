# Testing Strategy

## Test Layers

### Native Unit Tests

`tests_c/test_native.c` exercises configuration normalization and rejection,
path operations, queue behavior, atomic copy, destination symlink handling,
and shared platform helpers.

```bash
ctest --test-dir build -R native-unit --output-on-failure
```

### Linux Integration

`tests_c/integration_linux.sh` launches a real temporary service process and
checks:

- initial synchronization;
- inotify propagation and deletion-to-restore;
- same-size file with retrodated modification time;
- SQLite legacy schema migration;
- protected and unprotected HTTP behavior;
- token persistence and symlink rejection;
- remote bind refusal without authentication;
- restore listing/config APIs;
- traversal and symlink escape;
- second-process lock denial and kernel-backed status;
- clean shutdown and temporary-file cleanup.

It prints captured service stderr when the process exits unexpectedly.

### Windows Integration

`tests_c/integration_windows.ps1` starts the cross-compiled executable on
Windows, validates initial sync, Web authentication/status, event propagation,
text logging, and clean process termination. It opens the log with sharing so
the test reflects concurrent Windows service access.

### Maintenance Integration

`tests_c/maintenance_integration.sh` verifies stopped-state snapshots,
manifest/checksum generation, exact selector behavior, malicious archive
member rejection, and rollback behavior in an isolated fixture.

## Sanitizers

ASan plus UBSan runs all Linux CTest targets. A discovered heap use-after-free
in the restore POST strategy lifetime was fixed by converting parsed strategy
text to static enum names before deleting the cJSON tree.

TSan should run on native Linux CI. Certain WSL address layouts make the TSan
runtime abort before application code executes; that does not constitute a
passed race test.

## Static and Script Analysis

- GCC warnings: `-Wall -Wextra -Wpedantic -Wformat=2 -Wshadow -Wconversion`.
- `cppcheck --force` on first-party C.
- ShellCheck on Bash/package scripts.
- PowerShell parsing and Windows integration.
- `systemd-analyze verify` on the packaged unit.

## Release Acceptance Matrix

| Check | Linux | Windows | macOS |
|---|---|---|---|
| Compile Release | Required | Required | Both thin targets |
| Native unit | Execute | Execute | Cross-build only |
| Integration | Execute | Execute on Windows host | Manual target test |
| Binary format | ELF | PE32+ | Universal Mach-O |
| Package inspect | tar/DEB/RPM | ZIP/NSIS | DMG/HFS extraction |
| Hash manifest | Required | Required | Required |

macOS functional execution cannot be asserted by a Linux cross-builder. The
DMG and both Mach-O slices are structurally verified; launchd, filesystem
watch behavior, Gatekeeper, and restore must be manually tested on Intel and
Apple Silicon before public release.

## Manual Regression Checklist

1. Start with a copy of an old SQLite database.
2. Confirm one initial copy and one live modification.
3. Delete from source and locate the item in restore.
4. Restore with fail, rename, and overwrite strategies.
5. Save config and verify restart-required state.
6. Restart and verify new config becomes active.
7. Open CLI at narrow/wide terminal sizes.
8. Open Web UI at desktop and mobile widths.
9. Stop service during active queue and verify orderly termination.
10. Run maintenance snapshot, upgrade, and rollback.
