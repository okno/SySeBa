# **SySeBa – Syncro Service Backup**
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
| 📊 **Live dashboard** | Colour ASCII bars for disk/CPU/RAM usage |
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
# clone
git clone https://github.com/<your‑account>/syseba.git
cd syseba

# deps (virtualenv recommended)
pip install -r requirements.txt

# (optional) move to /opt
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
| Option | Description |
|--------|-------------|
| `--help` | Show built‑in help |
| `--silent` | Run without console UI (log only) |
| `--config PATH` | Alternate config file |
| `--lang it/en` | Interface language |
| `--create-daemon` | Generate & enable systemd unit |

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

Manually copy/move it from `restore` back to `source`; SySeBa will sync it on the next cycle.
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
 
