import argparse
import configparser
import hmac
import json
import logging
import mimetypes
import os
import secrets
import shlex
import shutil
import signal
import sqlite3
import subprocess
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from datetime import datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from queue import Empty, Queue
from urllib.parse import parse_qs, quote, unquote, urlparse

try:
    import psutil
except ImportError:
    psutil = None

try:
    from watchdog.events import FileSystemEventHandler
    from watchdog.observers import Observer
except ImportError:
    FileSystemEventHandler = object
    Observer = None


APP_NAME = "SySeBa"
APP_TITLE = "SySeBa - The Syncro Service Backup"
DEFAULT_LOCKFILE = "/opt/syseba/syseba.lock"
DEFAULT_DB_PATH = "/opt/syseba/syseba_logs.db"
DEFAULT_LANG_FILE = "/opt/syseba/syseba.lang"
DEFAULT_WEB_HOST = "127.0.0.1"
DEFAULT_WEB_PORT = 8765
DEFAULT_WEB_TOKEN_FILE = "/opt/syseba/syseba_web.token"
MAX_JSON_BODY = 64 * 1024
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")


@dataclass
class SySeBaConfig:
    source: str
    backup: str
    restore: str
    log_file: str
    threads: int
    config_path: str

    @classmethod
    def load(cls, config_path=None):
        path = find_config_file(config_path)
        parser = configparser.ConfigParser()
        parser.read(path, encoding="utf-8-sig")
        if not parser.has_section("SETTINGS"):
            raise ValueError("Invalid or missing [SETTINGS] section in config file.")

        settings = parser["SETTINGS"]
        base_dir = os.path.dirname(os.path.abspath(path))
        return cls(
            source=normalize_path(settings.get("source", "/dati"), base_dir),
            backup=normalize_path(settings.get("backup", "/backup"), base_dir),
            restore=normalize_path(settings.get("restore", "/restore"), base_dir),
            log_file=normalize_path(settings.get("log", "/var/log/syseba.log"), base_dir),
            threads=max(1, settings.getint("threads", 4)),
            config_path=os.path.abspath(path),
        )

    def save(self, values):
        updated = {
            "source": str(values.get("source", self.source)).strip(),
            "backup": str(values.get("backup", self.backup)).strip(),
            "restore": str(values.get("restore", self.restore)).strip(),
            "log": str(values.get("log_file", values.get("log", self.log_file))).strip(),
            "threads": str(values.get("threads", self.threads)).strip(),
        }

        if not all(updated[key] for key in ("source", "backup", "restore", "log")):
            raise ValueError("source, backup, restore and log cannot be empty.")

        try:
            threads = int(updated["threads"])
        except ValueError as exc:
            raise ValueError("threads must be a number.") from exc
        if threads < 1 or threads > 64:
            raise ValueError("threads must be between 1 and 64.")
        updated["threads"] = str(threads)

        content = (
            "#Backup config by okno\n"
            "[SETTINGS]\n"
            f"source = {updated['source']}\n"
            f"backup = {updated['backup']}\n"
            f"restore = {updated['restore']}\n"
            f"log = {updated['log']}\n"
            f"threads = {updated['threads']}\n"
        )
        with open(self.config_path, "w", encoding="utf-8") as config_file:
            config_file.write(content)

        return SySeBaConfig.load(self.config_path)

    def as_public_dict(self):
        return {
            "source": self.source,
            "backup": self.backup,
            "restore": self.restore,
            "log_file": self.log_file,
            "threads": self.threads,
            "config_path": self.config_path,
        }


@dataclass
class LogRecord:
    level: str
    message: str
    operation: str = "INFO"
    source_path: str = ""
    target_path: str = ""
    additional_info: str = ""

    def line(self):
        return f"{datetime.now().strftime('%Y-%m-%d %H:%M:%S')} [{self.level}] {self.message}"


def normalize_path(path, base_dir):
    expanded = os.path.expanduser(str(path).strip())
    if not os.path.isabs(expanded):
        expanded = os.path.join(base_dir, expanded)
    return os.path.abspath(expanded)


def find_config_file(config_path=None):
    possible_paths = []
    if config_path:
        possible_paths.append(config_path)
    possible_paths.extend([
        os.path.join(SCRIPT_DIR, "syseba.conf"),
        "syseba.conf",
        "/etc/syseba/syseba.conf",
        "/opt/syseba/syseba.conf",
    ])

    for path in possible_paths:
        if path and os.path.exists(path):
            return path
    raise FileNotFoundError("Config file not found in standard locations.")


def load_language(lang_file=DEFAULT_LANG_FILE, lang="it"):
    language_path = lang_file if os.path.exists(lang_file) else os.path.join(SCRIPT_DIR, "syseba.lang")
    labels = {
        "CLOCK": "Ora attuale" if lang == "it" else "Current time",
        "SPACE_USED": "Spazio usato" if lang == "it" else "Used disk space",
        "LOG": "Posizione file di LOG" if lang == "it" else "Log file location",
        "SHUTDOWN": "Chiusura in corso" if lang == "it" else "Shutting down",
    }

    if not os.path.exists(language_path):
        return labels

    with open(language_path, "r", encoding="utf-8-sig") as lang_file_handle:
        for line in lang_file_handle:
            parts = line.strip().split(";")
            if len(parts) == 3:
                label, italian, english = parts
                labels[label] = italian if lang == "it" else english
    return labels


def load_web_token(web_token=None, web_token_file=None, no_web_auth=False):
    if no_web_auth:
        return None, "disabled"

    if web_token:
        return web_token.strip(), "argument"

    env_token = os.environ.get("SYSEBA_WEB_TOKEN", "").strip()
    if env_token:
        return env_token, "environment"

    token_file = web_token_file or os.environ.get("SYSEBA_WEB_TOKEN_FILE", "").strip()
    if token_file and os.path.exists(token_file):
        with open(token_file, "r", encoding="utf-8") as token_handle:
            token = token_handle.readline().strip()
        if token:
            return token, "file"

    return secrets.token_urlsafe(32), "generated"


def ensure_dependencies():
    missing = []
    if psutil is None:
        missing.append("psutil")
    if Observer is None:
        missing.append("watchdog")
    if missing:
        joined = ", ".join(missing)
        raise RuntimeError(f"Missing dependencies: {joined}. Install them with: pip install -r requirements.txt")


def process_exists(pid):
    if pid <= 0:
        return False
    if psutil is not None:
        return psutil.pid_exists(pid)
    if os.name == "nt":
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def format_seconds(seconds):
    seconds = int(max(0, seconds))
    hours, remainder = divmod(seconds, 3600)
    minutes, secs = divmod(remainder, 60)
    return f"{hours:02d}:{minutes:02d}:{secs:02d}"


def bytes_to_human(size):
    value = float(size)
    for unit in ("B", "KB", "MB", "GB", "TB", "PB"):
        if value < 1024 or unit == "PB":
            return f"{value:.1f} {unit}"
        value /= 1024
    return f"{value:.1f} PB"


def disk_usage(path):
    if not os.path.exists(path):
        return {"exists": False, "path": path, "used_percent": None, "total": 0, "used": 0, "free": 0}
    usage = shutil.disk_usage(path)
    return {
        "exists": True,
        "path": path,
        "used_percent": round((usage.used / usage.total) * 100, 2) if usage.total else 0,
        "total": usage.total,
        "used": usage.used,
        "free": usage.free,
    }


def safe_join(base, relative_path):
    base_abs = os.path.abspath(base)
    raw_relative = unquote(str(relative_path or "")).strip()
    raw_relative = raw_relative.replace("\\", os.sep).replace("/", os.sep)
    normalized = os.path.normpath(raw_relative).lstrip(os.sep)
    if normalized in ("", "."):
        candidate = base_abs
    else:
        candidate = os.path.abspath(os.path.join(base_abs, normalized))

    if os.path.commonpath([base_abs, candidate]) != base_abs:
        raise ValueError("Invalid path.")
    if not is_inside_base(base_abs, nearest_existing_parent(candidate)):
        raise ValueError("Invalid path.")
    return candidate


def nearest_existing_parent(path):
    current = os.path.abspath(path)
    while not os.path.exists(current):
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
    return current


def is_inside_base(base, path):
    base_abs = os.path.realpath(os.path.abspath(base))
    path_abs = os.path.realpath(os.path.abspath(path))
    return os.path.commonpath([base_abs, path_abs]) == base_abs


def safe_download_name(path):
    name = os.path.basename(path).replace("\\", "_").replace("/", "_")
    name = name.replace('"', "_").replace("\r", "_").replace("\n", "_")
    return name or "download"


def relative_to_base(base, path):
    return os.path.relpath(os.path.abspath(path), os.path.abspath(base)).replace("\\", "/")


def unique_restore_path(path):
    if not os.path.exists(path):
        return path

    parent = os.path.dirname(path)
    name = os.path.basename(path)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    candidate = os.path.join(parent, f"{name}.{stamp}")
    counter = 1
    while os.path.exists(candidate):
        candidate = os.path.join(parent, f"{name}.{stamp}.{counter}")
        counter += 1
    return candidate


class ProcessLock:
    def __init__(self, path):
        self.path = path
        self.pid = os.getpid()
        self.acquired = False

    def acquire(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        if os.path.exists(self.path):
            try:
                with open(self.path, "r", encoding="utf-8") as lock_file:
                    old_pid = int(lock_file.read().strip())
                if process_exists(old_pid):
                    return False, old_pid
            except (ValueError, OSError):
                pass

        with open(self.path, "w", encoding="utf-8") as lock_file:
            lock_file.write(str(self.pid))
        self.acquired = True
        return True, self.pid

    def release(self):
        if not self.acquired or not os.path.exists(self.path):
            return
        try:
            with open(self.path, "r", encoding="utf-8") as lock_file:
                current_pid = int(lock_file.read().strip())
            if current_pid == self.pid:
                os.remove(self.path)
        except (ValueError, OSError):
            pass
        self.acquired = False


class SySeBaEventHandler(FileSystemEventHandler):
    def __init__(self, daemon):
        self.daemon = daemon

    def on_created(self, event):
        self.daemon.enqueue_event("create", event.src_path, event.is_directory)

    def on_modified(self, event):
        if not event.is_directory:
            self.daemon.enqueue_event("modify", event.src_path, False)

    def on_deleted(self, event):
        self.daemon.enqueue_event("delete", event.src_path, event.is_directory)

    def on_moved(self, event):
        self.daemon.enqueue_event("delete", event.src_path, event.is_directory)
        self.daemon.enqueue_event("create", event.dest_path, event.is_directory)


class SySeBaDaemon:
    LOG_COLUMNS = {
        "timestamp": "TEXT",
        "level": "TEXT DEFAULT 'INFO'",
        "operation": "TEXT",
        "source_path": "TEXT",
        "target_path": "TEXT",
        "additional_info": "TEXT",
    }

    def __init__(self, config, lang=None, lockfile=DEFAULT_LOCKFILE, db_path=DEFAULT_DB_PATH, no_initial_sync=False):
        self.config = config
        self.lang = lang or {}
        self.lockfile = lockfile
        self.db_path = db_path
        self.no_initial_sync = no_initial_sync
        self.start_time = time.time()
        self.stop_event = threading.Event()
        self.work_queue = Queue()
        self.log_queue = Queue()
        self.recent_logs = deque(maxlen=500)
        self.lock = threading.RLock()
        self.process_lock = ProcessLock(lockfile)
        self.observer = None
        self.log_thread = None
        self.worker_threads = []
        self.initial_sync_thread = None
        self.running = False
        self.web_only = False
        self.restart_required = False
        self.stats = {
            "queued_events": 0,
            "copied": 0,
            "updated": 0,
            "deleted": 0,
            "restored": 0,
            "skipped": 0,
            "errors": 0,
            "initial_total": 0,
            "initial_done": 0,
            "initial_copied": 0,
            "initial_skipped": 0,
            "initial_running": False,
        }

    def start(self):
        ensure_dependencies()
        self.prepare_paths()
        acquired, pid = self.process_lock.acquire()
        if not acquired:
            raise RuntimeError(f"SySeBa is already running with PID {pid}.")

        self.initialize_database()
        self.start_log_writer()

        for index in range(self.config.threads):
            worker = threading.Thread(target=self.worker_loop, name=f"syseba-worker-{index + 1}", daemon=True)
            worker.start()
            self.worker_threads.append(worker)

        self.observer = Observer()
        self.observer.schedule(SySeBaEventHandler(self), self.config.source, recursive=True)
        self.observer.start()

        if not self.no_initial_sync:
            self.initial_sync_thread = threading.Thread(target=self.initial_sync, name="syseba-initial-sync", daemon=True)
            self.initial_sync_thread.start()

        self.running = True
        self.emit("INFO", f"SySeBa started. Watching {self.config.source}", "START", self.config.source)

    def start_web_only(self):
        self.web_only = True
        self.prepare_log_path()
        self.initialize_database(allow_failure=True)
        self.start_log_writer()
        self.running = False
        self.emit("INFO", "Web dashboard started in web-only mode.", "WEB", self.config.config_path)

    def start_log_writer(self):
        if self.log_thread is not None and self.log_thread.is_alive():
            return
        self.log_thread = threading.Thread(target=self.log_writer, name="syseba-log-writer", daemon=True)
        self.log_thread.start()

    def prepare_paths(self):
        if not os.path.exists(self.config.source):
            raise FileNotFoundError(f"Source directory does not exist: {self.config.source}")
        os.makedirs(self.config.backup, exist_ok=True)
        os.makedirs(self.config.restore, exist_ok=True)
        self.prepare_log_path()

    def prepare_log_path(self):
        log_dir = os.path.dirname(self.config.log_file)
        if log_dir:
            os.makedirs(log_dir, exist_ok=True)

    def initialize_database(self, allow_failure=False):
        connection = None
        try:
            db_dir = os.path.dirname(self.db_path)
            if db_dir:
                os.makedirs(db_dir, exist_ok=True)
            connection = sqlite3.connect(self.db_path)
            connection.execute("PRAGMA journal_mode=WAL")
            self.ensure_log_schema(connection)
        except (OSError, sqlite3.Error):
            if not allow_failure:
                raise
        finally:
            if connection is not None:
                connection.close()

    def ensure_log_schema(self, connection):
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT,
                level TEXT,
                operation TEXT,
                source_path TEXT,
                target_path TEXT,
                additional_info TEXT
            )
            """
        )
        existing_columns = {
            row[1] for row in connection.execute("PRAGMA table_info(logs)").fetchall()
        }
        for column, definition in self.LOG_COLUMNS.items():
            if column not in existing_columns:
                connection.execute(f"ALTER TABLE logs ADD COLUMN {column} {definition}")
        connection.commit()

    def insert_log_record(self, cursor, record):
        cursor.execute(
            """
            INSERT INTO logs (
                timestamp, level, operation, source_path, target_path, additional_info
            ) VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                record.level,
                record.operation,
                record.source_path,
                record.target_path,
                record.additional_info,
            ),
        )

    def stop(self):
        self.emit("INFO", "SySeBa stopping.", "STOP")
        self.stop_event.set()

        if self.observer is not None:
            self.observer.stop()
            self.observer.join(timeout=10)

        if self.initial_sync_thread is not None and self.initial_sync_thread.is_alive():
            self.initial_sync_thread.join(timeout=10)

        self.wait_for_work_queue(timeout=30)

        for _ in self.worker_threads:
            self.work_queue.put(None)
        for worker in self.worker_threads:
            worker.join(timeout=5)

        self.log_queue.put(None)
        if self.log_thread is not None:
            self.log_thread.join(timeout=5)

        self.running = False
        self.process_lock.release()

    def wait_for_work_queue(self, timeout=30):
        deadline = time.time() + timeout
        while getattr(self.work_queue, "unfinished_tasks", 0) and time.time() < deadline:
            time.sleep(0.1)

    def enqueue_event(self, operation, path, is_directory=False):
        with self.lock:
            self.stats["queued_events"] += 1
        self.work_queue.put((operation, path, is_directory))

    def emit(self, level, message, operation="INFO", source_path="", target_path="", additional_info=""):
        record = LogRecord(level, message, operation, source_path or "", target_path or "", additional_info or "")
        line = record.line()
        with self.lock:
            self.recent_logs.append(line)
            if level == "ERROR":
                self.stats["errors"] += 1
        self.log_queue.put(record)

    def log_writer(self):
        connection = None
        cursor = None
        sqlite_error_reported = False
        try:
            with open(self.config.log_file, "a", encoding="utf-8") as log_file:
                try:
                    connection = sqlite3.connect(self.db_path)
                    self.ensure_log_schema(connection)
                    cursor = connection.cursor()
                except sqlite3.Error:
                    logging.exception("Unable to open SySeBa SQLite database. File logging continues.")
                    sqlite_error_reported = True

                while True:
                    record = self.log_queue.get()
                    if record is None:
                        self.log_queue.task_done()
                        break
                    try:
                        line = record.line()
                        log_file.write(line + "\n")
                        log_file.flush()
                        if cursor is None or connection is None:
                            continue
                        try:
                            self.insert_log_record(cursor, record)
                            connection.commit()
                            sqlite_error_reported = False
                        except sqlite3.OperationalError:
                            try:
                                connection.rollback()
                                self.ensure_log_schema(connection)
                                self.insert_log_record(cursor, record)
                                connection.commit()
                                sqlite_error_reported = False
                            except sqlite3.Error:
                                connection.rollback()
                                if not sqlite_error_reported:
                                    logging.exception("Unable to write SySeBa event to SQLite. File logging continues.")
                                    sqlite_error_reported = True
                        except sqlite3.Error:
                            connection.rollback()
                            if not sqlite_error_reported:
                                logging.exception("Unable to write SySeBa event to SQLite. File logging continues.")
                                sqlite_error_reported = True
                    finally:
                        self.log_queue.task_done()
        except (OSError, sqlite3.Error):
            logging.exception("Unable to write SySeBa log file.")
        finally:
            if connection is not None:
                connection.close()

    def initial_sync(self):
        self.set_stat("initial_running", True)
        try:
            total_files = sum(len(files) for _, _, files in os.walk(self.config.source))
            self.set_stat("initial_total", total_files)
            self.emit("INFO", f"Initial sync started. Files found: {total_files}", "SYNC", self.config.source)

            for root, _, files in os.walk(self.config.source):
                if self.stop_event.is_set():
                    break
                relative_path = os.path.relpath(root, self.config.source)
                backup_path = self.config.backup if relative_path == "." else os.path.join(self.config.backup, relative_path)
                os.makedirs(backup_path, exist_ok=True)

                for filename in files:
                    if self.stop_event.is_set():
                        break
                    src_file = os.path.join(root, filename)
                    dest_file = os.path.join(backup_path, filename)
                    try:
                        if self.needs_copy(src_file, dest_file):
                            self.copy_with_retry(src_file, dest_file)
                            self.increment("initial_copied")
                            self.increment("copied")
                            self.emit("INFO", f"Initial copy: {src_file} -> {dest_file}", "COPY", src_file, dest_file)
                        else:
                            self.increment("initial_skipped")
                            self.increment("skipped")
                    except Exception as exc:
                        self.emit("ERROR", f"Initial sync error for {src_file}: {exc}", "ERROR", src_file, dest_file, str(exc))
                    finally:
                        self.increment("initial_done")

            self.emit("INFO", "Initial sync completed.", "SYNC", self.config.source)
        finally:
            self.set_stat("initial_running", False)

    def worker_loop(self):
        while True:
            try:
                item = self.work_queue.get(timeout=0.5)
            except Empty:
                if self.stop_event.is_set():
                    continue
                continue
            if item is None:
                self.work_queue.task_done()
                break

            operation, src_path, is_directory = item
            try:
                self.process_event(operation, src_path, is_directory)
            except Exception as exc:
                self.emit("ERROR", f"Error processing {operation} for {src_path}: {exc}", "ERROR", src_path, "", str(exc))
            finally:
                self.work_queue.task_done()

    def process_event(self, operation, src_path, is_directory=False):
        if not is_inside_base(self.config.source, src_path):
            return
        relative_path = os.path.relpath(os.path.abspath(src_path), os.path.abspath(self.config.source))

        backup_path = os.path.join(self.config.backup, relative_path)
        restore_path = os.path.join(self.config.restore, relative_path)

        if operation == "create":
            if is_directory:
                os.makedirs(backup_path, exist_ok=True)
                self.emit("INFO", f"Directory created: {backup_path}", "MKDIR", src_path, backup_path)
                return
            self.copy_with_retry(src_path, backup_path)
            self.increment("copied")
            self.emit("INFO", f"Created: {src_path} -> {backup_path}", "CREATE", src_path, backup_path)
            return

        if operation == "modify":
            if os.path.exists(src_path):
                time.sleep(0.15)
                self.copy_with_retry(src_path, backup_path)
                self.increment("updated")
                self.emit("INFO", f"Modified: {src_path} -> {backup_path}", "MODIFY", src_path, backup_path)
            return

        if operation == "delete":
            if os.path.exists(backup_path):
                destination = unique_restore_path(restore_path)
                os.makedirs(os.path.dirname(destination), exist_ok=True)
                shutil.move(backup_path, destination)
                self.increment("deleted")
                self.emit("INFO", f"Deleted from source, moved backup to restore: {destination}", "DELETE", backup_path, destination)

    def copy_with_retry(self, src_file, dest_file, attempts=4, delay=0.25):
        last_error = None
        for attempt in range(attempts):
            try:
                if not os.path.exists(src_file):
                    raise FileNotFoundError(src_file)
                os.makedirs(os.path.dirname(dest_file), exist_ok=True)
                shutil.copy2(src_file, dest_file)
                return
            except OSError as exc:
                last_error = exc
                if attempt < attempts - 1:
                    time.sleep(delay * (attempt + 1))
        raise last_error

    def needs_copy(self, src_file, dest_file):
        if not os.path.exists(dest_file):
            return True
        try:
            src_stat = os.stat(src_file)
            dest_stat = os.stat(dest_file)
        except OSError:
            return True
        return src_stat.st_size != dest_stat.st_size or int(src_stat.st_mtime) > int(dest_stat.st_mtime)

    def restore_item(self, relative_path, overwrite=False):
        source_item = safe_join(self.config.restore, relative_path)
        if not os.path.exists(source_item):
            raise FileNotFoundError("Restore item not found.")

        destination = safe_join(self.config.source, relative_path)
        if os.path.exists(destination) and not overwrite:
            raise FileExistsError("Destination already exists. Enable overwrite to restore it.")

        os.makedirs(os.path.dirname(destination), exist_ok=True)
        if os.path.isdir(source_item):
            shutil.copytree(source_item, destination, dirs_exist_ok=overwrite)
        else:
            shutil.copy2(source_item, destination)

        self.increment("restored")
        self.emit("INFO", f"Restored from restore area: {source_item} -> {destination}", "RESTORE", source_item, destination)
        return destination

    def list_restore(self, relative_path=""):
        restore_root = self.config.restore
        target = safe_join(restore_root, relative_path)
        if not os.path.exists(target):
            raise FileNotFoundError("Restore path not found.")
        if not os.path.isdir(target):
            stat = os.stat(target)
            return {
                "path": relative_to_base(restore_root, target),
                "is_file": True,
                "size": stat.st_size,
                "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="seconds"),
                "items": [],
            }

        items = []
        for entry in os.scandir(target):
            try:
                stat = entry.stat()
            except OSError:
                continue
            items.append({
                "name": entry.name,
                "path": relative_to_base(restore_root, entry.path),
                "is_dir": entry.is_dir(),
                "size": stat.st_size,
                "size_human": bytes_to_human(stat.st_size),
                "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="seconds"),
            })
        items.sort(key=lambda item: (not item["is_dir"], item["name"].lower()))
        return {
            "path": relative_to_base(restore_root, target) if target != os.path.abspath(restore_root) else "",
            "is_file": False,
            "items": items[:1000],
        }

    def get_config_from_disk(self):
        return SySeBaConfig.load(self.config.config_path)

    def update_config(self, values):
        updated = self.config.save(values)
        self.restart_required = True
        self.emit("INFO", "Configuration updated from web interface. Restart SySeBa to apply runtime changes.", "CONFIG", self.config.config_path)
        return updated

    def status(self):
        cpu_percent = None
        memory_mb = None
        pid = os.getpid()
        if psutil is not None:
            try:
                process = psutil.Process(pid)
                cpu_percent = process.cpu_percent(interval=0.05)
                memory_mb = round(process.memory_info().rss / (1024 ** 2), 2)
            except psutil.Error:
                pass

        with self.lock:
            stats = dict(self.stats)
            recent_logs = list(self.recent_logs)[-20:]

        initial_total = stats["initial_total"]
        initial_done = stats["initial_done"]
        initial_percent = round((initial_done / initial_total) * 100, 2) if initial_total else 100.0

        return {
            "app": APP_NAME,
            "running": self.running,
            "web_only": self.web_only,
            "restart_required": self.restart_required,
            "pid": pid,
            "lockfile": self.lockfile,
            "db_path": self.db_path,
            "uptime": format_seconds(time.time() - self.start_time),
            "started_at": datetime.fromtimestamp(self.start_time).isoformat(timespec="seconds"),
            "now": datetime.now().isoformat(timespec="seconds"),
            "config": self.config.as_public_dict(),
            "disk": {
                "source": disk_usage(self.config.source),
                "backup": disk_usage(self.config.backup),
                "restore": disk_usage(self.config.restore),
            },
            "process": {
                "cpu_percent": cpu_percent,
                "memory_mb": memory_mb,
                "threads": len(threading.enumerate()),
                "queue_size": self.work_queue.qsize(),
            },
            "stats": stats,
            "initial_sync_percent": initial_percent,
            "recent_logs": recent_logs,
            "external_lock": self.external_lock_status(),
        }

    def external_lock_status(self):
        if not os.path.exists(self.lockfile):
            return {"exists": False, "pid": None, "running": False}
        try:
            with open(self.lockfile, "r", encoding="utf-8") as lock_file:
                pid = int(lock_file.read().strip())
            return {"exists": True, "pid": pid, "running": process_exists(pid)}
        except (ValueError, OSError):
            return {"exists": True, "pid": None, "running": False}

    def set_stat(self, key, value):
        with self.lock:
            self.stats[key] = value

    def increment(self, key, amount=1):
        with self.lock:
            self.stats[key] += amount


class SySeBaHTTPHandler(BaseHTTPRequestHandler):
    daemon_ref = None
    auth_token = None
    auth_source = "disabled"

    @property
    def daemon(self):
        return self.daemon_ref

    def log_message(self, format_string, *args):
        logging.info("web %s - %s", self.address_string(), format_string % args)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        try:
            if path == "/":
                self.send_html(render_dashboard_html())
            elif path == "/logo":
                self.send_file(os.path.join(SCRIPT_DIR, "SySeBa_Logo.webp"))
            elif not self.require_auth():
                return
            elif path == "/api/status":
                self.send_json(self.daemon.status())
            elif path == "/api/logs":
                lines = int(query.get("lines", ["200"])[0])
                self.send_json({"lines": tail_file(self.daemon.config.log_file, max(1, min(lines, 2000)))})
            elif path == "/api/config":
                self.send_json(self.daemon.get_config_from_disk().as_public_dict())
            elif path == "/api/restore":
                relative_path = query.get("path", [""])[0]
                self.send_json(self.daemon.list_restore(relative_path))
            elif path == "/restore/download":
                relative_path = query.get("path", [""])[0]
                target = safe_join(self.daemon.config.restore, relative_path)
                if not os.path.isfile(target):
                    self.send_error_json(HTTPStatus.NOT_FOUND, "File not found.")
                else:
                    self.send_file(target, download=True)
            else:
                self.send_error_json(HTTPStatus.NOT_FOUND, "Not found.")
        except Exception as exc:
            self.send_error_json(HTTPStatus.INTERNAL_SERVER_ERROR, str(exc))

    def do_POST(self):
        parsed = urlparse(self.path)
        try:
            if not self.require_auth():
                return
            data = self.read_json()
            if parsed.path == "/api/config":
                updated = self.daemon.update_config(data)
                self.send_json({
                    "ok": True,
                    "restart_required": True,
                    "config": updated.as_public_dict(),
                    "message": "Configuration saved. Restart SySeBa to apply it.",
                })
            elif parsed.path == "/api/restore":
                restored_to = self.daemon.restore_item(data.get("path", ""), bool(data.get("overwrite", False)))
                self.send_json({"ok": True, "restored_to": restored_to})
            else:
                self.send_error_json(HTTPStatus.NOT_FOUND, "Not found.")
        except Exception as exc:
            self.send_error_json(HTTPStatus.BAD_REQUEST, str(exc))

    def read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length > MAX_JSON_BODY:
            raise ValueError("Request body too large.")
        body = self.rfile.read(length).decode("utf-8") if length else "{}"
        return json.loads(body or "{}")

    def require_auth(self):
        if not self.auth_token:
            return True

        supplied = self.headers.get("X-SySeBa-Token", "").strip()
        if not supplied:
            authorization = self.headers.get("Authorization", "").strip()
            if authorization.lower().startswith("bearer "):
                supplied = authorization[7:].strip()

        if supplied and hmac.compare_digest(supplied, self.auth_token):
            return True

        self.send_json(
            {"ok": False, "error": "Authentication required."},
            HTTPStatus.UNAUTHORIZED,
            extra_headers={"WWW-Authenticate": 'Bearer realm="SySeBa"'},
        )
        return False

    def send_security_headers(self):
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self' 'unsafe-inline'; style-src 'self' 'unsafe-inline'; "
            "img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'self'",
        )

    def send_json(self, payload, status=HTTPStatus.OK, extra_headers=None):
        data = json.dumps(payload, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_security_headers()
        for key, value in (extra_headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)

    def send_html(self, html_body):
        data = html_body.encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_security_headers()
        self.end_headers()
        self.wfile.write(data)

    def send_error_json(self, status, message):
        self.send_json({"ok": False, "error": message}, status)

    def send_file(self, path, download=False):
        if not os.path.exists(path):
            self.send_error_json(HTTPStatus.NOT_FOUND, "File not found.")
            return
        mime_type = mimetypes.guess_type(path)[0] or "application/octet-stream"
        with open(path, "rb") as file_handle:
            data = file_handle.read()
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", mime_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_security_headers()
        if download:
            filename = safe_download_name(path)
            quoted = quote(filename)
            self.send_header("Content-Disposition", f'attachment; filename="{filename}"; filename*=UTF-8\'\'{quoted}')
        self.end_headers()
        self.wfile.write(data)


def tail_file(path, lines=200):
    if not os.path.exists(path):
        return []
    try:
        block_size = 8192
        chunks = []
        newlines = 0
        with open(path, "rb") as log_file:
            log_file.seek(0, os.SEEK_END)
            position = log_file.tell()
            while position > 0 and newlines <= lines:
                read_size = min(block_size, position)
                position -= read_size
                log_file.seek(position)
                data = log_file.read(read_size)
                chunks.append(data)
                newlines += data.count(b"\n")
        content = b"".join(reversed(chunks)).decode("utf-8", errors="replace")
        return content.splitlines()[-lines:]
    except OSError:
        return []


def render_dashboard_html():
    return """<!doctype html>
<html lang="it">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SySeBa</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f4f1ea;
      --surface: #ffffff;
      --surface-strong: #232826;
      --text: #1b1f1d;
      --muted: #66706b;
      --line: #d8d2c7;
      --accent: #27745f;
      --accent-2: #b85c38;
      --danger: #b42318;
      --warn: #b7791f;
      --ok: #1f8a57;
      --code: #101615;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      color: var(--text);
      background: var(--bg);
    }
    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 16px;
      padding: 18px 24px;
      background: var(--surface-strong);
      color: white;
      border-bottom: 4px solid var(--accent);
    }
    .brand { display: flex; align-items: center; gap: 14px; min-width: 0; }
    .brand img { width: 44px; height: 44px; object-fit: contain; background: white; border-radius: 6px; }
    h1 { margin: 0; font-size: 24px; line-height: 1.1; letter-spacing: 0; }
    .subtitle { color: #cfd8d3; font-size: 13px; margin-top: 4px; }
    .pill {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      padding: 7px 10px;
      border: 1px solid rgba(255,255,255,.18);
      border-radius: 6px;
      font-size: 13px;
      background: rgba(255,255,255,.08);
      white-space: nowrap;
    }
    .auth-panel {
      background: #fff7e6;
      border-bottom: 1px solid #e7c98a;
      padding: 14px 24px;
    }
    .auth-panel .auth-inner {
      display: flex;
      align-items: end;
      gap: 10px;
      max-width: 1440px;
      margin: 0 auto;
      flex-wrap: wrap;
    }
    .auth-panel label { min-width: min(420px, 100%); }
    main { padding: 22px; max-width: 1440px; margin: 0 auto; }
    nav {
      display: flex;
      gap: 8px;
      border-bottom: 1px solid var(--line);
      margin-bottom: 18px;
      overflow-x: auto;
    }
    nav button {
      border: 0;
      border-bottom: 3px solid transparent;
      background: transparent;
      padding: 12px 12px 10px;
      font: inherit;
      color: var(--muted);
      cursor: pointer;
    }
    nav button.active { color: var(--text); border-color: var(--accent); font-weight: 700; }
    .grid { display: grid; gap: 14px; }
    .grid.cols-4 { grid-template-columns: repeat(4, minmax(0, 1fr)); }
    .grid.cols-3 { grid-template-columns: repeat(3, minmax(0, 1fr)); }
    .panel {
      background: var(--surface);
      border: 1px solid var(--line);
      border-radius: 8px;
      padding: 16px;
      min-width: 0;
    }
    .panel h2 { margin: 0 0 14px; font-size: 16px; letter-spacing: 0; }
    .metric-label { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: .06em; }
    .metric-value { margin-top: 6px; font-size: 28px; font-weight: 800; line-height: 1; }
    .metric-small { margin-top: 6px; color: var(--muted); font-size: 13px; overflow-wrap: anywhere; }
    .bar { height: 9px; background: #e8e2d7; border-radius: 999px; overflow: hidden; margin-top: 10px; }
    .bar span { display: block; height: 100%; width: 0; background: var(--accent); }
    .bar.warn span { background: var(--warn); }
    .bar.danger span { background: var(--danger); }
    table { width: 100%; border-collapse: collapse; }
    th, td { padding: 10px 8px; text-align: left; border-bottom: 1px solid var(--line); font-size: 14px; }
    th { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .06em; }
    code, pre {
      font-family: "Cascadia Mono", "SFMono-Regular", Consolas, monospace;
      background: var(--code);
      color: #e8fff4;
      border-radius: 6px;
    }
    pre { padding: 14px; overflow: auto; max-height: 520px; line-height: 1.45; }
    label { display: grid; gap: 6px; font-size: 13px; color: var(--muted); }
    input {
      width: 100%;
      border: 1px solid var(--line);
      border-radius: 6px;
      padding: 10px 11px;
      font: inherit;
      color: var(--text);
      background: white;
    }
    .form-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; }
    .actions { display: flex; align-items: center; gap: 10px; margin-top: 14px; flex-wrap: wrap; }
    button.primary, button.secondary {
      border: 0;
      border-radius: 6px;
      padding: 10px 13px;
      font: inherit;
      font-weight: 700;
      cursor: pointer;
    }
    button.primary { color: white; background: var(--accent); }
    button.secondary { color: var(--text); background: #e7e0d5; }
    .notice { margin-top: 10px; color: var(--accent-2); font-weight: 700; }
    .hide { display: none; }
    .path { overflow-wrap: anywhere; }
    .status-ok { color: var(--ok); font-weight: 800; }
    .status-bad { color: var(--danger); font-weight: 800; }
    .restore-row button { padding: 6px 9px; border-radius: 6px; border: 1px solid var(--line); background: white; cursor: pointer; }
    @media (max-width: 1000px) {
      .grid.cols-4, .grid.cols-3, .form-grid { grid-template-columns: 1fr; }
      header { align-items: flex-start; flex-direction: column; }
    }
  </style>
</head>
<body>
  <header>
    <div class="brand">
      <img src="/logo" alt="SySeBa">
      <div>
        <h1>SySeBa</h1>
        <div class="subtitle">Syncro Service Backup</div>
      </div>
    </div>
    <div class="pill" id="runtime-pill">Caricamento stato...</div>
  </header>
  <section class="auth-panel hide" id="auth-panel">
    <div class="auth-inner">
      <label>Token web SySeBa<input id="auth-token" type="password" autocomplete="current-password"></label>
      <button class="primary" id="auth-save">Accedi</button>
      <button class="secondary" id="auth-clear">Dimentica token</button>
      <div class="notice" id="auth-notice"></div>
    </div>
  </section>
  <main>
    <nav>
      <button class="active" data-tab="status">Stato</button>
      <button data-tab="logs">Log</button>
      <button data-tab="config">Configurazione</button>
      <button data-tab="restore">Restore</button>
    </nav>

    <section id="tab-status" class="tab">
      <div class="grid cols-4" id="metrics"></div>
      <div class="grid cols-3" style="margin-top:14px" id="disk"></div>
      <div class="panel" style="margin-top:14px">
        <h2>Attivita</h2>
        <table><tbody id="activity"></tbody></table>
      </div>
    </section>

    <section id="tab-logs" class="tab hide">
      <div class="panel">
        <h2>Log</h2>
        <pre id="logs">Caricamento...</pre>
      </div>
    </section>

    <section id="tab-config" class="tab hide">
      <div class="panel">
        <h2>Configurazione</h2>
        <form id="config-form" class="form-grid">
          <label>Source<input name="source"></label>
          <label>Backup<input name="backup"></label>
          <label>Restore<input name="restore"></label>
          <label>Log<input name="log_file"></label>
          <label>Threads<input name="threads" type="number" min="1" max="64"></label>
        </form>
        <div class="actions">
          <button class="primary" id="save-config">Salva configurazione</button>
          <button class="secondary" id="reload-config">Ricarica</button>
        </div>
        <div class="notice" id="config-notice"></div>
      </div>
    </section>

    <section id="tab-restore" class="tab hide">
      <div class="panel">
        <h2>Area restore</h2>
        <div class="actions">
          <button class="secondary" id="restore-up">Su</button>
          <code id="restore-path" style="padding:9px 10px"></code>
        </div>
        <table style="margin-top:12px">
          <thead><tr><th>Nome</th><th>Tipo</th><th>Dimensione</th><th>Modifica</th><th>Azione</th></tr></thead>
          <tbody id="restore-list"></tbody>
        </table>
      </div>
    </section>
  </main>
  <script>
    const tabs = document.querySelectorAll('nav button');
    let currentRestorePath = '';
    let lastConfig = null;
    let authToken = sessionStorage.getItem('sysebaAuthToken') || '';

    tabs.forEach(button => button.addEventListener('click', () => {
      tabs.forEach(item => item.classList.remove('active'));
      document.querySelectorAll('.tab').forEach(tab => tab.classList.add('hide'));
      button.classList.add('active');
      document.getElementById(`tab-${button.dataset.tab}`).classList.remove('hide');
      if (button.dataset.tab === 'logs') loadLogs();
      if (button.dataset.tab === 'config') loadConfig();
      if (button.dataset.tab === 'restore') loadRestore(currentRestorePath);
    }));

    function escapeHtml(value) {
      return String(value ?? '').replace(/[&<>"']/g, char => ({
        '&': '&amp;',
        '<': '&lt;',
        '>': '&gt;',
        '"': '&quot;',
        "'": '&#39;'
      })[char]);
    }

    function authHeaders() {
      return authToken ? {'X-SySeBa-Token': authToken} : {};
    }

    function showAuth(message = 'Token richiesto per consultare e amministrare SySeBa.') {
      document.getElementById('auth-panel').classList.remove('hide');
      document.getElementById('auth-notice').textContent = message;
      document.getElementById('auth-token').value = authToken;
    }

    function hideAuth() {
      document.getElementById('auth-panel').classList.add('hide');
      document.getElementById('auth-notice').textContent = '';
    }

    async function request(path, options = {}) {
      const headers = {
        'Content-Type': 'application/json',
        ...authHeaders(),
        ...(options.headers || {})
      };
      const response = await fetch(path, {
        ...options,
        headers
      });
      if (response.status === 401) {
        showAuth('Token non valido o mancante.');
        throw new Error('Autenticazione richiesta');
      }
      const data = await response.json();
      if (!response.ok || data.ok === false) throw new Error(data.error || 'Errore richiesta');
      hideAuth();
      return data;
    }

    function pct(value) {
      if (value === null || value === undefined) return 'n/d';
      return `${Number(value).toFixed(2)}%`;
    }

    function metric(label, value, detail = '') {
      return `<div class="panel"><div class="metric-label">${escapeHtml(label)}</div><div class="metric-value">${escapeHtml(value)}</div><div class="metric-small">${escapeHtml(detail)}</div></div>`;
    }

    function diskPanel(label, item) {
      const value = item.exists ? item.used_percent : 0;
      const tone = value > 90 ? 'danger' : value > 75 ? 'warn' : '';
      return `<div class="panel"><h2>${escapeHtml(label)}</h2><div class="path">${escapeHtml(item.path)}</div><div class="bar ${tone}"><span style="width:${value}%"></span></div><div class="metric-small">${item.exists ? `${pct(value)} usato` : 'Percorso non trovato'}</div></div>`;
    }

    async function loadStatus() {
      try {
        const status = await request('/api/status');
        document.getElementById('runtime-pill').innerHTML = status.running ? '<span class="status-ok">RUNNING</span> ' + escapeHtml(status.uptime) : '<span class="status-bad">WEB ONLY</span> ' + escapeHtml(status.uptime);
        document.getElementById('metrics').innerHTML = [
          metric('PID', status.pid, status.config.config_path),
          metric('CPU', pct(status.process.cpu_percent), `${status.process.threads} thread processo`),
          metric('RAM', `${status.process.memory_mb ?? 'n/d'} MB`, `Coda: ${status.process.queue_size}`),
          metric('Sync iniziale', pct(status.initial_sync_percent), status.stats.initial_running ? 'in corso' : 'ferma')
        ].join('');
        document.getElementById('disk').innerHTML = [
          diskPanel('Source', status.disk.source),
          diskPanel('Backup', status.disk.backup),
          diskPanel('Restore', status.disk.restore)
        ].join('');
        const s = status.stats;
        document.getElementById('activity').innerHTML = `
          <tr><th>Copiati</th><td>${escapeHtml(s.copied)}</td><th>Aggiornati</th><td>${escapeHtml(s.updated)}</td></tr>
          <tr><th>Cancellati in restore</th><td>${escapeHtml(s.deleted)}</td><th>Ripristinati</th><td>${escapeHtml(s.restored)}</td></tr>
          <tr><th>Saltati</th><td>${escapeHtml(s.skipped)}</td><th>Errori</th><td>${escapeHtml(s.errors)}</td></tr>
          <tr><th>Eventi ricevuti</th><td>${escapeHtml(s.queued_events)}</td><th>Restart config</th><td>${status.restart_required ? 'richiesto' : 'no'}</td></tr>`;
      } catch (error) {
        document.getElementById('runtime-pill').textContent = error.message;
      }
    }

    async function loadLogs() {
      const data = await request('/api/logs?lines=250');
      document.getElementById('logs').textContent = data.lines.join('\\n') || 'Nessun log disponibile.';
    }

    async function loadConfig() {
      lastConfig = await request('/api/config');
      for (const [key, value] of Object.entries(lastConfig)) {
        const input = document.querySelector(`[name="${key}"]`);
        if (input) input.value = value;
      }
    }

    async function saveConfig() {
      const form = document.getElementById('config-form');
      const data = Object.fromEntries(new FormData(form).entries());
      const result = await request('/api/config', {method: 'POST', body: JSON.stringify(data)});
      document.getElementById('config-notice').textContent = result.message;
      await loadConfig();
      await loadStatus();
    }

    async function loadRestore(path = '') {
      currentRestorePath = path;
      const data = await request('/api/restore?path=' + encodeURIComponent(path));
      document.getElementById('restore-path').textContent = '/' + (data.path || '');
      const rows = data.items.map(item => {
        const open = item.is_dir
          ? `<button data-action="open" data-path="${escapeHtml(item.path)}">Apri</button>`
          : `<button data-action="download" data-path="${escapeHtml(item.path)}">Download</button>`;
        const restore = `<button data-action="restore" data-path="${escapeHtml(item.path)}">Ripristina</button>`;
        return `<tr class="restore-row"><td class="path">${escapeHtml(item.name)}</td><td>${item.is_dir ? 'dir' : 'file'}</td><td>${escapeHtml(item.size_human)}</td><td>${escapeHtml(item.mtime)}</td><td>${open} ${restore}</td></tr>`;
      }).join('');
      document.getElementById('restore-list').innerHTML = rows || '<tr><td colspan="5">Area restore vuota.</td></tr>';
    }

    async function restoreItem(path) {
      if (!confirm('Ripristinare questo elemento nella source?')) return;
      await request('/api/restore', {method: 'POST', body: JSON.stringify({path, overwrite: false})});
      await loadRestore(currentRestorePath);
      await loadStatus();
    }

    async function downloadItem(path) {
      const response = await fetch('/restore/download?path=' + encodeURIComponent(path), {headers: authHeaders()});
      if (response.status === 401) {
        showAuth('Token non valido o mancante.');
        return;
      }
      if (!response.ok) throw new Error('Download non riuscito');
      const blob = await response.blob();
      const url = URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = path.split('/').pop() || 'download';
      document.body.appendChild(link);
      link.click();
      link.remove();
      URL.revokeObjectURL(url);
    }

    document.getElementById('save-config').addEventListener('click', saveConfig);
    document.getElementById('reload-config').addEventListener('click', loadConfig);
    document.getElementById('auth-save').addEventListener('click', async () => {
      authToken = document.getElementById('auth-token').value.trim();
      sessionStorage.setItem('sysebaAuthToken', authToken);
      await loadStatus();
    });
    document.getElementById('auth-clear').addEventListener('click', () => {
      authToken = '';
      sessionStorage.removeItem('sysebaAuthToken');
      showAuth('Token dimenticato.');
    });
    document.getElementById('restore-list').addEventListener('click', event => {
      const button = event.target.closest('button[data-action]');
      if (!button) return;
      const path = button.dataset.path || '';
      if (button.dataset.action === 'open') loadRestore(path);
      if (button.dataset.action === 'download') downloadItem(path);
      if (button.dataset.action === 'restore') restoreItem(path);
    });
    document.getElementById('restore-up').addEventListener('click', () => {
      const parts = currentRestorePath.split('/').filter(Boolean);
      parts.pop();
      loadRestore(parts.join('/'));
    });
    loadStatus();
    setInterval(loadStatus, 3000);
  </script>
</body>
</html>
"""


class ConsoleDashboard:
    def __init__(self, daemon, refresh_seconds=3):
        self.daemon = daemon
        self.refresh_seconds = refresh_seconds

    def run(self):
        try:
            while not self.daemon.stop_event.is_set():
                self.render()
                self.daemon.stop_event.wait(self.refresh_seconds)
        except KeyboardInterrupt:
            self.daemon.stop_event.set()

    def render(self):
        status = self.daemon.status()
        width = shutil.get_terminal_size((100, 30)).columns
        print("\033[2J\033[H", end="")
        print(color("=" * min(width, 100), "cyan"))
        print(color(" SySeBa", "red") + "  The Syncro Service Backup".ljust(max(1, width - 8)))
        print(color("=" * min(width, 100), "cyan"))
        runtime = "RUNNING" if status["running"] else "WEB ONLY"
        print(f" Stato: {color(runtime, 'green' if status['running'] else 'yellow')} | PID: {status['pid']} | Uptime: {status['uptime']} | Ora: {datetime.now().strftime('%H:%M:%S')}")
        if status["restart_required"]:
            print(color(" Configurazione modificata: riavvia SySeBa per applicarla al watcher.", "yellow"))
        print()
        print(color(" Percorsi", "cyan"))
        self.print_disk("SOURCE ", status["disk"]["source"], width)
        self.print_disk("BACKUP ", status["disk"]["backup"], width)
        self.print_disk("RESTORE", status["disk"]["restore"], width)
        print()
        print(color(" Processo", "cyan"))
        process = status["process"]
        print(f" CPU: {value_or_na(process['cpu_percent'], '%')} | RAM: {value_or_na(process['memory_mb'], ' MB')} | Thread: {process['threads']} | Coda: {process['queue_size']}")
        stats = status["stats"]
        print(f" Copiati: {stats['copied']} | Aggiornati: {stats['updated']} | Cancellati->restore: {stats['deleted']} | Ripristinati: {stats['restored']} | Errori: {stats['errors']}")
        sync_state = "in corso" if stats["initial_running"] else "ferma"
        print(f" Sync iniziale: {status['initial_sync_percent']:.2f}% ({stats['initial_done']}/{stats['initial_total']}) | {sync_state}")
        print()
        print(color(" Log", "cyan"))
        print(f" {status['config']['log_file']}")
        print()
        print(color(" Ultimi eventi", "cyan"))
        for line in status["recent_logs"][-8:]:
            print(" " + truncate(line, width - 2))
        print()
        print(color(" Ctrl+C per uscire in modo pulito.", "cyan"))

    def print_disk(self, label, item, width):
        if not item["exists"]:
            print(f" {label}: {item['path']} | {color('non trovato', 'red')}")
            return
        percent = item["used_percent"]
        bar_width = min(44, max(20, width - 55))
        print(f" {label}: {truncate(item['path'], 34)} | {colored_bar(percent, bar_width)} {percent:6.2f}%")


def value_or_na(value, suffix=""):
    if value is None:
        return "n/d"
    if isinstance(value, float):
        return f"{value:.2f}{suffix}"
    return f"{value}{suffix}"


def truncate(value, width):
    text = str(value)
    if len(text) <= width:
        return text
    return text[: max(0, width - 3)] + "..."


def color(text, name):
    colors = {
        "red": "\033[91m",
        "green": "\033[92m",
        "yellow": "\033[93m",
        "cyan": "\033[96m",
        "reset": "\033[0m",
    }
    return colors.get(name, "") + str(text) + colors["reset"]


def colored_bar(percent, width):
    filled = int((percent / 100) * width)
    raw = "#" * filled + "-" * (width - filled)
    if percent >= 90:
        return color(raw, "red")
    if percent >= 75:
        return color(raw, "yellow")
    return color(raw, "green")


def start_web_server(daemon, host, port, auth_token=None, auth_source="disabled"):
    SySeBaHTTPHandler.daemon_ref = daemon
    SySeBaHTTPHandler.auth_token = auth_token
    SySeBaHTTPHandler.auth_source = auth_source
    server = ThreadingHTTPServer((host, port), SySeBaHTTPHandler)
    thread = threading.Thread(target=server.serve_forever, name="syseba-web", daemon=True)
    thread.start()
    daemon.emit("INFO", f"Web interface listening on http://{host}:{port}", "WEB")
    if auth_token:
        daemon.emit("INFO", f"Web authentication enabled ({auth_source} token).", "WEB")
    else:
        daemon.emit("WARNING", "Web authentication disabled by explicit request.", "WEB")
    return server


def create_systemd_service(
    config_path=None,
    with_web=False,
    web_host=DEFAULT_WEB_HOST,
    web_port=DEFAULT_WEB_PORT,
    web_token_file=None,
    no_web_auth=False,
):
    command = ["/usr/bin/python3", "/opt/syseba/syseba.py", "--silent"]
    if config_path:
        command.extend(["--config", config_path])
    if with_web:
        command.extend(["--web", "--web-host", web_host, "--web-port", str(web_port)])
        if web_token_file:
            command.extend(["--web-token-file", web_token_file])
        if no_web_auth:
            command.append("--no-web-auth")
    exec_start = " ".join(shlex.quote(part) for part in command)
    service_content = f"""[Unit]
Description={APP_TITLE}
After=network.target

[Service]
ExecStart={exec_start}
WorkingDirectory=/opt/syseba
Restart=always
RestartSec=5
User=root
Group=root
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=read-only
ReadWritePaths=/opt/syseba /var/log -/storage -/backup -/restore -/dati -/mnt -/media -/srv
UMask=0077

[Install]
WantedBy=multi-user.target
"""

    with open("/etc/systemd/system/syseba.service", "w", encoding="utf-8") as service_file:
        service_file.write(service_content)
    subprocess.run(["systemctl", "daemon-reload"], check=False)
    subprocess.run(["systemctl", "enable", "syseba.service"], check=False)
    print("Systemd service created and enabled: syseba.service")


def build_parser():
    parser = argparse.ArgumentParser(description=APP_TITLE)
    parser.add_argument("--create-daemon", action="store_true", help="Create and enable SySeBa as a systemd service")
    parser.add_argument("--silent", action="store_true", help="Run without console dashboard")
    parser.add_argument("--config", help="Specify custom config file path")
    parser.add_argument("--lang", choices=["it", "en"], default="it", help="Select language")
    parser.add_argument("--web", action="store_true", help="Start the web interface together with the daemon")
    parser.add_argument("--web-only", action="store_true", help="Start only the web interface without file watcher")
    parser.add_argument("--web-host", default=DEFAULT_WEB_HOST, help="Web interface host")
    parser.add_argument("--web-port", type=int, default=DEFAULT_WEB_PORT, help="Web interface port")
    parser.add_argument("--web-token", help="Token required by the web interface")
    parser.add_argument("--web-token-file", default=DEFAULT_WEB_TOKEN_FILE, help="File containing the web token")
    parser.add_argument("--no-web-auth", action="store_true", help="Disable web authentication (unsafe)")
    parser.add_argument("--no-initial-sync", action="store_true", help="Skip initial sync at startup")
    parser.add_argument("--lockfile", default=DEFAULT_LOCKFILE, help="Lock file path")
    parser.add_argument("--db-path", default=DEFAULT_DB_PATH, help="SQLite database path")
    parser.add_argument("--console-refresh", type=float, default=3.0, help="Console dashboard refresh interval")
    return parser


def main():
    parser = build_parser()
    args = parser.parse_args()

    if args.create_daemon:
        create_systemd_service(
            args.config,
            args.web,
            args.web_host,
            args.web_port,
            args.web_token_file,
            args.no_web_auth,
        )
        return

    config = SySeBaConfig.load(args.config)
    lang = load_language(lang=args.lang)
    daemon = SySeBaDaemon(config, lang=lang, lockfile=args.lockfile, db_path=args.db_path, no_initial_sync=args.no_initial_sync)
    web_server = None

    def request_stop(signum=None, frame=None):
        daemon.stop_event.set()
        if web_server is not None:
            web_server.shutdown()

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)

    try:
        if args.web_only:
            daemon.start_web_only()
        else:
            daemon.start()

        if args.web or args.web_only:
            web_token, web_token_source = load_web_token(args.web_token, args.web_token_file, args.no_web_auth)
            web_server = start_web_server(daemon, args.web_host, args.web_port, web_token, web_token_source)
            print(f"Web interface: http://{args.web_host}:{args.web_port}")
            if web_token:
                if web_token_source == "generated":
                    print(f"Web token: {web_token}")
                else:
                    print(f"Web token source: {web_token_source}")
            else:
                print("WARNING: web authentication is disabled.")

        if not args.silent and not args.web_only:
            ConsoleDashboard(daemon, args.console_refresh).run()
        else:
            while not daemon.stop_event.is_set():
                daemon.stop_event.wait(1)
    except KeyboardInterrupt:
        daemon.stop_event.set()
    except Exception as exc:
        logging.error(str(exc))
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
    finally:
        if web_server is not None:
            web_server.shutdown()
            web_server.server_close()
        daemon.stop()


if __name__ == "__main__":
    main()
