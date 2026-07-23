# Operations and Observability

## Service Control

Linux:

```bash
systemctl status syseba.service --no-pager -l
systemctl restart syseba.service
systemctl stop syseba.service
systemctl cat syseba.service
```

Windows:

```powershell
Get-Service SySeBa
Restart-Service SySeBa
sc.exe qc SySeBa
```

macOS:

```bash
sudo launchctl print system/com.okno.syseba
sudo launchctl kickstart -k system/com.okno.syseba
```

## Console Logs for a Service

The interactive dashboard is intentionally disabled in service mode. To see
startup, failures, signal handling, and server binding on Linux:

```bash
sudo journalctl -fu syseba.service -o short-iso-precise
```

To follow file operations:

```bash
sudo tail -n 200 -F /var/log/syseba/syseba.log
```

Both streams:

```bash
sudo ./scripts/syseba-maintenance.sh follow
```

Historical boot:

```bash
sudo journalctl -u syseba.service -b -n 300 --no-pager
```

## Health Checks

```bash
syseba status
syseba status --json
curl -fsS http://127.0.0.1:8765/api/auth
ss -lntp | grep ':8765'
```

`/api/auth` proves the HTTP listener is responsive without disclosing status
or requiring the bearer token.

## Important Metrics

- `initial_sync`: startup reconciliation state and progress.
- `watcher`: backend health.
- `queue_size`: pending filesystem operations.
- `queued_events`, `copied`, `deleted`, `restored`, `errors`.
- elapsed time, process CPU, resident memory.
- used/free space for source, backup, and restore.
- saved/active configuration difference.

A continuously growing queue indicates insufficient storage throughput,
unbounded source churn, permission failures with retries, or too few workers.
High CPU with low throughput can indicate millions of tiny files or excessive
worker count.

## Runbook: Backup Not Advancing

1. `syseba config-check --json`.
2. Check service active state and duplicate-instance status.
3. Check free space and mount read/write state.
4. Inspect the first error in application log, not only repeated follow-ups.
5. Confirm watcher state and queue depth.
6. Confirm source files are stable long enough to copy.
7. Restart only after collecting the relevant journal and log tail.

## Runbook: Web UI Unreachable

1. Verify unit `ExecStart` contains `--web`.
2. Verify listener on port 8765.
3. Test `/api/auth` from the host.
4. Test the server IP from the LAN.
5. Check host firewall and network ACL.
6. Check port conflicts and journal bind errors.

## Runbook: SQLite Error

Stop any old Python process and ensure systemd launches the C executable:

```bash
systemctl cat syseba.service | grep ExecStart
ps aux | grep '[s]yseba'
syseba --version
```

Restart once. The migration runs before the log writer. Preserve DB, `-wal`,
and `-shm` together if taking a manual stopped-state copy.

## Capacity Planning

Restore consumption can grow without bound because deleted backup content is
retained. Monitor restore separately from backup. Define retention outside
SySeBa only after business and recovery requirements are explicit; this
release intentionally does not auto-delete restore history.

## Configuration Changes

Use Web UI or edit the INI, then:

```bash
syseba config-check
sudo systemctl restart syseba.service
```

Keep a software snapshot before changing storage roots. A root change starts a
new reconciliation and does not migrate old backup/restore content.
