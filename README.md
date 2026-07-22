# **SySeBa – Syncro Service Backup**
![SySeBa Logo](https://github.com/okno/SySeBa/blob/main/SySeBa_Logo.webp)

*A lightweight, Anti Ransomware, Real‑Time, File‑Sync & Backup daemon for Linux*

## ✨ Overview
**SySeBa** is a Python-based daemon that provides real-time replication of a designated source directory to a backup target, while preserving deleted files in a structured restore path instead of removing them. 

It is designed as a lightweight, agentless Continuous Data Protection (CDP) solution optimized for Linux environments.

In the current threat landscape — where ransomware, insider threats, and unauthorized file manipulation are rising sharply — maintaining a secure, tamper-resilient backup workflow is no longer optional. 

Traditional periodic backups leave critical time gaps where data can be lost or encrypted without fallback. 

SySeBa addresses this by offering:
- Immediate file-level replication, reducing Recovery Point Objective (RPO) to near-zero;
- Soft-deletion safeguards, enabling forensic traceability and easy rollback from human or malicious deletions;
- SQLite-based audit logging and real-time disk usage monitoring to improve situational awareness.
  
By integrating seamlessly with systemd and operating without requiring external agents or network exposure, SySeBa helps enforce data resilience policies even in air-gapped or hardened environments.

## 🚀 Features
|  |  |
|---|---|
| 📁 **Initial full sync** | Multithreaded copy of existing data |
| 👁️ **Real‑time watch** | *watchdog* library hooks inotify events |
| 🗑️ **Soft‑delete** | Removed files are relocated to `/restore` |
| 📝 **Dual logging** | Flat‑file **&** SQLite (`/opt/syseba/syseba_logs.db`) |
| 📊 **Adaptive console** | Responsive status view for narrow and short terminals |
| 🌐 **Protected web UI** | Accessible, responsive status, filtered logs, config diff, and restore browser |
| ♻️ **Guided restore** | Conflict detection with safe rename or explicit overwrite/merge |
| 🛠️ **One‑shot systemd unit** | `--create-daemon` generates & enables service |
| 🌐 **Multi‑language** | Italian & English shipped (`syseba.lang`) |

## 📋 Requirements
| Package | Min Version | Install hint |
|---------|-------------|--------------|
| `python` | 3.8 | tested up to 3.12 |
| `watchdog` | 3.0 | `pip install watchdog` |
| `psutil` | 5.9 | `pip install psutil` |


## ⚡ Quick Start
```bash
# Clone
git clone https://github.com/okno/SySeBa.git
cd SySeBa

# Deps
pip install -r requirements.txt

# Move to /opt
sudo mkdir -p /opt/syseba
sudo cp -r . /opt/syseba
```

### Configuration
Create `/etc/syseba/syseba.conf`:
```ini
[SETTINGS]
source  = /data
backup  = /backup
restore = /restore
log     = /var/log/syseba.log
threads = 4
```
*(Optional)* edit `/opt/syseba/syseba.lang` for localisation (`LABEL;IT;EN`).

### Manual run
```bash
sudo python3 /opt/syseba/syseba.py --lang en
```

### Systemd service
```bash
sudo python3 /opt/syseba/syseba.py --create-daemon
sudo systemctl start syseba
sudo systemctl status syseba
```

## 🛠️ CLI Reference
Operational commands remain optional, so existing invocations continue to start the daemon.

| Command | Description |
|---------|-------------|
| `run` | Start the watcher; this is the default when no command is supplied |
| `status` | Inspect lock state, PID, paths, and disk usage |
| `logs` | Print the latest log lines |
| `config-check` | Validate paths and detect unsafe directory overlap |
| `restore-list` | Search and page through the restore area |
| `restore-copy --path PATH` | Restore an item; add `--rename` or `--overwrite` for conflicts |
| `service-install` | Generate and enable the systemd unit |

| Option | Description |
|--------|-------------|
| `--help` | Show built‑in help |
| `--silent` | Run without console UI (log only) |
| `--config PATH` | Alternate config file |
| `--lang it/en` | Interface language |
| `--web` | Start token-protected web dashboard |
| `--web-token TOKEN` | Web dashboard token |
| `--web-token-file PATH` | Read web dashboard token from file |
| `--no-web-auth` | Disable web authentication for local lab use only |
| `--create-daemon` | Generate & enable systemd unit |
| `--json` | Machine-readable output for operational commands |

## ❓ FAQ
<details>
<summary><strong>What if two instances start?</strong></summary>

A lock file at `/opt/syseba/syseba.lock` prevents duplicates; the second process exits.
</details>

<details>
<summary><strong>Where do deleted files go?</strong></summary>

They are **moved**, not removed, to the `restore` directory preserving structure & timestamps.
</details>

<details>
<summary><strong>Can I replace SQLite?</strong></summary>

Yes – extend `initialize_database()` and `log_to_database()` for your preferred DB engine.
</details>

<details>
<summary><strong>How do I restore a file?</strong></summary>

Use the Web UI Restore tab or `syseba.py restore-copy --path relative/file`. If the destination already exists, choose a timestamped new name or explicitly overwrite/merge it.
</details>

<details>
<summary><strong>Is it resource‑hungry?</strong></summary>

Average CPU usage is **< 1 %**, with spikes only during the first bulk sync.
</details>

## 🤝 Contributing
Pull requests are welcome! Please open an issue first to discuss major changes.

## 🪪 License
MIT – see `LICENSE`.

> Crafted with ❤️ by *okno* & gpt-o3-pro
 
