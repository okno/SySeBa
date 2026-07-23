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
import stat
import subprocess
import sys
import tempfile
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
DEFAULT_SERVICE_WEB_HOST = "0.0.0.0"
DEFAULT_WEB_PORT = 8765
DEFAULT_WEB_TOKEN_FILE = "/opt/syseba/syseba_web.token"
DEFAULT_SYSTEMD_SERVICE = "syseba.service"
DEFAULT_SYSTEMD_UNIT = "/etc/systemd/system/syseba.service"
DEFAULT_INSTALL_DIR = "/opt/syseba"
MAX_JSON_BODY = 64 * 1024
DEFAULT_RESTORE_PAGE_SIZE = 100
MAX_RESTORE_PAGE_SIZE = 250
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
    def load(cls, config_path=None, language="en"):
        italian = language == "it"
        path = find_config_file(config_path, language)
        parser = configparser.ConfigParser()
        parser.read(path, encoding="utf-8-sig")
        if not parser.has_section("SETTINGS"):
            raise ValueError(
                "Sezione [SETTINGS] non valida o mancante nel file di configurazione."
                if italian
                else "Invalid or missing [SETTINGS] section in config file."
            )

        settings = parser["SETTINGS"]
        base_dir = os.path.dirname(os.path.abspath(path))
        try:
            threads = settings.getint("threads", 4)
        except ValueError as exc:
            raise ValueError(
                "Il numero di thread deve essere un intero."
                if italian
                else "Threads must be an integer."
            ) from exc
        if threads < 1 or threads > 64:
            raise ValueError(
                "Il numero di thread deve essere compreso tra 1 e 64."
                if italian
                else "Threads must be between 1 and 64."
            )
        return cls(
            source=normalize_path(settings.get("source", "/dati"), base_dir),
            backup=normalize_path(settings.get("backup", "/backup"), base_dir),
            restore=normalize_path(settings.get("restore", "/restore"), base_dir),
            log_file=normalize_path(settings.get("log", "/var/log/syseba.log"), base_dir),
            threads=threads,
            config_path=os.path.abspath(path),
        )

    def save(self, values, language="en"):
        italian = language == "it"
        updated = {
            "source": str(values.get("source", self.source)).strip(),
            "backup": str(values.get("backup", self.backup)).strip(),
            "restore": str(values.get("restore", self.restore)).strip(),
            "log": str(values.get("log_file", values.get("log", self.log_file))).strip(),
            "threads": str(values.get("threads", self.threads)).strip(),
        }

        if not all(updated[key] for key in ("source", "backup", "restore", "log")):
            raise ValueError(
                "Sorgente, backup, restore e log non possono essere vuoti."
                if italian
                else "Source, backup, restore, and log cannot be empty."
            )

        try:
            threads = int(updated["threads"])
        except ValueError as exc:
            raise ValueError(
                "Il numero di thread deve essere un intero."
                if italian
                else "Threads must be an integer."
            ) from exc
        if threads < 1 or threads > 64:
            raise ValueError(
                "Il numero di thread deve essere compreso tra 1 e 64."
                if italian
                else "Threads must be between 1 and 64."
            )
        updated["threads"] = str(threads)

        base_dir = os.path.dirname(os.path.abspath(self.config_path))
        candidate = SySeBaConfig(
            source=normalize_path(updated["source"], base_dir),
            backup=normalize_path(updated["backup"], base_dir),
            restore=normalize_path(updated["restore"], base_dir),
            log_file=normalize_path(updated["log"], base_dir),
            threads=threads,
            config_path=os.path.abspath(self.config_path),
        )
        errors, _ = validate_config(candidate, language)
        if errors:
            raise ValueError("; ".join(errors))

        content = (
            "#Backup config by okno\n"
            "[SETTINGS]\n"
            f"source = {updated['source']}\n"
            f"backup = {updated['backup']}\n"
            f"restore = {updated['restore']}\n"
            f"log = {updated['log']}\n"
            f"threads = {updated['threads']}\n"
        )
        atomic_write_text(self.config_path, content, mode=0o640)

        return SySeBaConfig.load(self.config_path, language)

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


def find_config_file(config_path=None, language="en"):
    if config_path:
        if os.path.isfile(config_path):
            return config_path
        raise FileNotFoundError(
            f"File di configurazione specificato non trovato: {config_path}"
            if language == "it"
            else f"Specified config file not found: {config_path}"
        )

    possible_paths = [
        os.path.join(SCRIPT_DIR, "syseba.conf"),
        "syseba.conf",
        "/etc/syseba/syseba.conf",
        "/opt/syseba/syseba.conf",
    ]

    for path in possible_paths:
        if path and os.path.exists(path):
            return path
    raise FileNotFoundError(
        "File di configurazione non trovato nei percorsi standard."
        if language == "it"
        else "Config file not found in standard locations."
    )


def load_language(lang_file=DEFAULT_LANG_FILE, lang="it"):
    language_path = lang_file if os.path.exists(lang_file) else os.path.join(SCRIPT_DIR, "syseba.lang")
    defaults = {
        "it": {
            "CLOCK": "Ora",
            "SPACE_USED": "Spazio usato",
            "LOG": "Log",
            "SHUTDOWN": "Chiusura in corso",
            "STATUS": "Stato",
            "PATHS": "Percorsi",
            "PROCESS": "Processo",
            "RECENT_EVENTS": "Ultimi eventi",
            "RUNNING": "ATTIVO",
            "STOPPED": "FERMO",
            "WEB_ONLY": "SOLO WEB",
            "SOURCE": "SORGENTE",
            "BACKUP": "BACKUP",
            "RESTORE": "RESTORE",
            "COPIED": "Copiati",
            "UPDATED": "Aggiornati",
            "DELETED": "In restore",
            "RESTORED": "Ripristinati",
            "ERRORS": "Errori",
            "QUEUE": "Coda",
            "THREADS": "Thread",
            "INITIAL_SYNC": "Sincronizzazione iniziale",
            "SYNC_PENDING": "in attesa",
            "SYNC_RUNNING": "in corso",
            "SYNC_COMPLETED": "completata",
            "SYNC_COMPLETED_WITH_ERRORS": "completata con errori",
            "SYNC_SKIPPED": "saltata",
            "SYNC_STOPPED": "interrotta",
            "SYNC_FAILED": "fallita",
            "SYNC_NOT_AVAILABLE": "non disponibile",
            "CONFIG_RESTART": "Configurazione salvata: riavvio necessario.",
            "CLEAN_EXIT": "Ctrl+C per uscire in modo pulito.",
            "NOT_FOUND": "non trovato",
            "NO_EVENTS": "Nessun evento recente.",
            "WARNING": "AVVISO",
            "ERROR": "ERRORE",
            "FILE": "FILE",
            "DIRECTORY": "CARTELLA",
            "ITEMS": "elementi",
            "PAGE": "Pagina",
            "DESTINATION_EXISTS": "destinazione esistente",
        },
        "en": {
            "CLOCK": "Time",
            "SPACE_USED": "Used space",
            "LOG": "Log",
            "SHUTDOWN": "Shutting down",
            "STATUS": "Status",
            "PATHS": "Paths",
            "PROCESS": "Process",
            "RECENT_EVENTS": "Recent events",
            "RUNNING": "RUNNING",
            "STOPPED": "STOPPED",
            "WEB_ONLY": "WEB ONLY",
            "SOURCE": "SOURCE",
            "BACKUP": "BACKUP",
            "RESTORE": "RESTORE",
            "COPIED": "Copied",
            "UPDATED": "Updated",
            "DELETED": "In restore",
            "RESTORED": "Restored",
            "ERRORS": "Errors",
            "QUEUE": "Queue",
            "THREADS": "Threads",
            "INITIAL_SYNC": "Initial sync",
            "SYNC_PENDING": "pending",
            "SYNC_RUNNING": "running",
            "SYNC_COMPLETED": "completed",
            "SYNC_COMPLETED_WITH_ERRORS": "completed with errors",
            "SYNC_SKIPPED": "skipped",
            "SYNC_STOPPED": "stopped",
            "SYNC_FAILED": "failed",
            "SYNC_NOT_AVAILABLE": "not available",
            "CONFIG_RESTART": "Configuration saved: restart required.",
            "CLEAN_EXIT": "Ctrl+C to exit cleanly.",
            "NOT_FOUND": "not found",
            "NO_EVENTS": "No recent events.",
            "WARNING": "WARNING",
            "ERROR": "ERROR",
            "FILE": "FILE",
            "DIRECTORY": "DIR",
            "ITEMS": "items",
            "PAGE": "Page",
            "DESTINATION_EXISTS": "destination exists",
        },
    }
    labels = dict(defaults.get(lang, defaults["it"]))

    if not os.path.exists(language_path):
        return labels

    with open(language_path, "r", encoding="utf-8-sig") as lang_file_handle:
        for line in lang_file_handle:
            parts = line.strip().split(";")
            if len(parts) == 3:
                label, italian, english = parts
                labels[label] = italian if lang == "it" else english
    return labels


def atomic_write_text(path, content, mode=0o644):
    target = os.path.abspath(os.path.expanduser(path))
    directory = os.path.dirname(target) or "."
    os.makedirs(directory, exist_ok=True)
    file_descriptor, temporary_path = tempfile.mkstemp(
        prefix=f".{os.path.basename(target)}.",
        dir=directory,
        text=True,
    )
    try:
        os.fchmod(file_descriptor, mode)
        with os.fdopen(file_descriptor, "w", encoding="utf-8", newline="\n") as output:
            file_descriptor = None
            output.write(content)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, target)
        os.chmod(target, mode)
    finally:
        if file_descriptor is not None:
            os.close(file_descriptor)
        if os.path.exists(temporary_path):
            os.unlink(temporary_path)
    return target


def read_private_token_file(path, language="en"):
    flags = os.O_RDONLY
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    file_descriptor = os.open(path, flags)
    try:
        metadata = os.fstat(file_descriptor)
        if not stat.S_ISREG(metadata.st_mode):
            raise ValueError(
                f"Il percorso del token Web non è un file regolare: {path}"
                if language == "it"
                else f"Web token path is not a regular file: {path}"
            )
        with os.fdopen(file_descriptor, "r", encoding="utf-8") as token_handle:
            file_descriptor = None
            return token_handle.readline().strip()
    finally:
        if file_descriptor is not None:
            os.close(file_descriptor)


def ensure_web_token_file(path=DEFAULT_WEB_TOKEN_FILE, language="en"):
    token_path = os.path.abspath(os.path.expanduser(path))
    if os.path.lexists(token_path):
        if os.path.islink(token_path):
            raise ValueError(
                f"File token Web rifiutato perché è un link simbolico: {token_path}"
                if language == "it"
                else f"Refusing symbolic-link Web token file: {token_path}"
            )
        token = read_private_token_file(token_path, language)
        if token:
            os.chmod(token_path, 0o600)
            return token, token_path, False

    token = secrets.token_urlsafe(32)
    atomic_write_text(token_path, token + "\n", mode=0o600)
    return token, token_path, True


def load_web_token(web_token=None, web_token_file=None, no_web_auth=False, language="en"):
    if no_web_auth:
        return None, "disabled"

    if web_token:
        return web_token.strip(), "argument"

    env_token = os.environ.get("SYSEBA_WEB_TOKEN", "").strip()
    if env_token:
        return env_token, "environment"

    token_file = (
        web_token_file
        or os.environ.get("SYSEBA_WEB_TOKEN_FILE", "").strip()
        or DEFAULT_WEB_TOKEN_FILE
    )
    token, _, created = ensure_web_token_file(token_file, language)
    return token, "generated-file" if created else "file"


def ensure_dependencies(language="en"):
    missing = []
    if psutil is None:
        missing.append("psutil")
    if Observer is None:
        missing.append("watchdog")
    if missing:
        joined = ", ".join(missing)
        raise RuntimeError(
            (
                f"Dipendenze mancanti: {joined}. Installale con: "
                "pip install -r requirements.txt"
            )
            if language == "it"
            else f"Missing dependencies: {joined}. Install them with: pip install -r requirements.txt"
        )


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


def unique_restored_path(path):
    if not os.path.exists(path):
        return path

    parent = os.path.dirname(path)
    name = os.path.basename(path)
    stem, extension = os.path.splitext(name)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    candidate = os.path.join(parent, f"{stem}.restored-{stamp}{extension}")
    counter = 1
    while os.path.exists(candidate):
        candidate = os.path.join(parent, f"{stem}.restored-{stamp}-{counter}{extension}")
        counter += 1
    return candidate


class ProcessLock:
    def __init__(self, path):
        self.path = path
        self.pid = os.getpid()
        self.acquired = False

    def acquire(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
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

    def __init__(
        self,
        config,
        lang=None,
        lockfile=DEFAULT_LOCKFILE,
        db_path=DEFAULT_DB_PATH,
        no_initial_sync=False,
        language="it",
    ):
        self.config = config
        self.lang = lang or {}
        self.language = language if language in ("it", "en") else "it"
        self.lockfile = lockfile
        self.db_path = db_path
        self.no_initial_sync = no_initial_sync
        self.start_time = time.time()
        self.last_event_at = None
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
        self.initial_sync_state = "skipped" if no_initial_sync else "pending"
        self.initial_sync_started_at = None
        self.initial_sync_completed_at = None
        self.initial_sync_error = None
        self.systemd_service = os.environ.get("SYSEBA_SYSTEMD_SERVICE", DEFAULT_SYSTEMD_SERVICE).strip() or DEFAULT_SYSTEMD_SERVICE
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

    def message(self, italian, english):
        return italian if self.language == "it" else english

    def start(self):
        ensure_dependencies(self.language)
        self.prepare_paths()
        acquired, pid = self.process_lock.acquire()
        if not acquired:
            raise RuntimeError(
                self.message(
                    f"SySeBa è già in esecuzione con PID {pid}.",
                    f"SySeBa is already running with PID {pid}.",
                )
            )

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
        self.emit(
            "INFO",
            self.message(
                f"SySeBa avviato. Monitoraggio di {self.config.source}",
                f"SySeBa started. Watching {self.config.source}",
            ),
            "START",
            self.config.source,
        )

    def start_web_only(self):
        self.web_only = True
        self.initial_sync_state = "not_available"
        self.prepare_log_path()
        self.initialize_database(allow_failure=True)
        self.start_log_writer()
        self.running = False
        self.emit(
            "INFO",
            self.message(
                "Dashboard Web avviata in modalità solo Web.",
                "Web dashboard started in web-only mode.",
            ),
            "WEB",
            self.config.config_path,
        )

    def start_log_writer(self):
        if self.log_thread is not None and self.log_thread.is_alive():
            return
        self.log_thread = threading.Thread(target=self.log_writer, name="syseba-log-writer", daemon=True)
        self.log_thread.start()

    def prepare_paths(self):
        if not os.path.exists(self.config.source):
            raise FileNotFoundError(
                self.message(
                    f"La directory sorgente non esiste: {self.config.source}",
                    f"Source directory does not exist: {self.config.source}",
                )
            )
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
        self.emit("INFO", self.message("Arresto di SySeBa.", "SySeBa stopping."), "STOP")
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
            self.last_event_at = datetime.now().isoformat(timespec="seconds")
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
                    logging.exception(
                        self.message(
                            "Impossibile aprire il database SQLite di SySeBa. Il log su file continua.",
                            "Unable to open SySeBa SQLite database. File logging continues.",
                        )
                    )
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
                                    logging.exception(
                                        self.message(
                                            "Impossibile scrivere l'evento SySeBa in SQLite. Il log su file continua.",
                                            "Unable to write SySeBa event to SQLite. File logging continues.",
                                        )
                                    )
                                    sqlite_error_reported = True
                        except sqlite3.Error:
                            connection.rollback()
                            if not sqlite_error_reported:
                                logging.exception(
                                    self.message(
                                        "Impossibile scrivere l'evento SySeBa in SQLite. Il log su file continua.",
                                        "Unable to write SySeBa event to SQLite. File logging continues.",
                                    )
                                )
                                sqlite_error_reported = True
                    finally:
                        self.log_queue.task_done()
        except (OSError, sqlite3.Error):
            logging.exception(
                self.message(
                    "Impossibile scrivere il file di log di SySeBa.",
                    "Unable to write SySeBa log file.",
                )
            )
        finally:
            if connection is not None:
                connection.close()

    def initial_sync(self):
        sync_errors = 0
        self.set_initial_sync_state("running")
        self.set_stat("initial_running", True)
        try:
            total_files = sum(len(files) for _, _, files in os.walk(self.config.source))
            self.set_stat("initial_total", total_files)
            self.emit(
                "INFO",
                self.message(
                    f"Sincronizzazione iniziale avviata. File trovati: {total_files}",
                    f"Initial sync started. Files found: {total_files}",
                ),
                "SYNC",
                self.config.source,
            )

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
                            self.emit(
                                "INFO",
                                self.message(
                                    f"Copia iniziale: {src_file} -> {dest_file}",
                                    f"Initial copy: {src_file} -> {dest_file}",
                                ),
                                "COPY",
                                src_file,
                                dest_file,
                            )
                        else:
                            self.increment("initial_skipped")
                            self.increment("skipped")
                    except Exception as exc:
                        sync_errors += 1
                        self.emit(
                            "ERROR",
                            self.message(
                                f"Errore di sincronizzazione iniziale per {src_file}: {exc}",
                                f"Initial sync error for {src_file}: {exc}",
                            ),
                            "ERROR",
                            src_file,
                            dest_file,
                            str(exc),
                        )
                    finally:
                        self.increment("initial_done")

            if self.stop_event.is_set():
                self.set_initial_sync_state("stopped")
                self.emit(
                    "WARNING",
                    self.message(
                        "Sincronizzazione iniziale interrotta prima del completamento.",
                        "Initial sync stopped before completion.",
                    ),
                    "SYNC",
                    self.config.source,
                )
            else:
                state = "completed_with_errors" if sync_errors else "completed"
                self.set_initial_sync_state(state)
                self.emit(
                    "INFO",
                    self.message("Sincronizzazione iniziale completata.", "Initial sync completed."),
                    "SYNC",
                    self.config.source,
                )
        except Exception as exc:
            self.set_initial_sync_state("failed", str(exc))
            self.emit(
                "ERROR",
                self.message(
                    f"Sincronizzazione iniziale fallita: {exc}",
                    f"Initial sync failed: {exc}",
                ),
                "SYNC",
                self.config.source,
                "",
                str(exc),
            )
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
                self.emit(
                    "ERROR",
                    self.message(
                        f"Errore durante l'operazione {operation} su {src_path}: {exc}",
                        f"Error processing {operation} for {src_path}: {exc}",
                    ),
                    "ERROR",
                    src_path,
                    "",
                    str(exc),
                )
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
                self.emit(
                    "INFO",
                    self.message(
                        f"Directory creata: {backup_path}",
                        f"Directory created: {backup_path}",
                    ),
                    "MKDIR",
                    src_path,
                    backup_path,
                )
                return
            if not os.path.exists(src_path):
                self.increment("skipped")
                return
            try:
                self.copy_with_retry(src_path, backup_path)
            except FileNotFoundError:
                self.increment("skipped")
                return
            self.increment("copied")
            self.emit(
                "INFO",
                self.message(
                    f"Creato: {src_path} -> {backup_path}",
                    f"Created: {src_path} -> {backup_path}",
                ),
                "CREATE",
                src_path,
                backup_path,
            )
            return

        if operation == "modify":
            if os.path.exists(src_path):
                time.sleep(0.15)
                if not os.path.exists(src_path):
                    self.increment("skipped")
                    return
                try:
                    self.copy_with_retry(src_path, backup_path)
                except FileNotFoundError:
                    self.increment("skipped")
                    return
                self.increment("updated")
                self.emit(
                    "INFO",
                    self.message(
                        f"Modificato: {src_path} -> {backup_path}",
                        f"Modified: {src_path} -> {backup_path}",
                    ),
                    "MODIFY",
                    src_path,
                    backup_path,
                )
            return

        if operation == "delete":
            if os.path.exists(backup_path):
                destination = unique_restore_path(restore_path)
                os.makedirs(os.path.dirname(destination), exist_ok=True)
                shutil.move(backup_path, destination)
                self.increment("deleted")
                self.emit(
                    "INFO",
                    self.message(
                        f"Eliminato dalla sorgente; backup spostato nel restore: {destination}",
                        f"Deleted from source, moved backup to restore: {destination}",
                    ),
                    "DELETE",
                    backup_path,
                    destination,
                )

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

    def restore_item(self, relative_path, overwrite=False, strategy=None):
        source_item = safe_join(self.config.restore, relative_path)
        if os.path.normcase(source_item) == os.path.normcase(os.path.abspath(self.config.restore)):
            raise ValueError(self.message("Il percorso dell'elemento restore non può essere vuoto.", "Restore item path cannot be empty."))
        if not os.path.exists(source_item):
            raise FileNotFoundError(self.message("Elemento restore non trovato.", "Restore item not found."))

        destination = safe_join(self.config.source, relative_path)
        strategy = strategy or ("overwrite" if overwrite else "fail")
        if strategy not in ("fail", "overwrite", "rename"):
            raise ValueError(self.message("Strategia di restore non valida.", "Invalid restore strategy."))

        destination_exists = os.path.exists(destination)
        if destination_exists and strategy == "rename":
            destination = unique_restored_path(destination)
            destination_exists = False
        elif destination_exists and strategy == "fail":
            raise FileExistsError(
                self.message(
                    "La destinazione esiste già. Scegli rinomina o sovrascrivi.",
                    "Destination already exists. Choose rename or overwrite.",
                )
            )

        if destination_exists and os.path.isdir(source_item) != os.path.isdir(destination):
            raise FileExistsError(
                self.message(
                    "Il tipo della destinazione è diverso dall'elemento restore. Scegli rinomina.",
                    "Destination type differs from restore item. Choose rename.",
                )
            )

        os.makedirs(os.path.dirname(destination), exist_ok=True)
        if os.path.isdir(source_item):
            shutil.copytree(source_item, destination, dirs_exist_ok=strategy == "overwrite")
        else:
            shutil.copy2(source_item, destination)

        self.increment("restored")
        self.emit(
            "INFO",
            self.message(
                f"Ripristinato dall'area restore: {source_item} -> {destination}",
                f"Restored from restore area: {source_item} -> {destination}",
            ),
            "RESTORE",
            source_item,
            destination,
        )
        return destination

    def restore_item_info(self, relative_path):
        source_item = safe_join(self.config.restore, relative_path)
        if os.path.normcase(source_item) == os.path.normcase(os.path.abspath(self.config.restore)):
            raise ValueError(self.message("Il percorso dell'elemento restore non può essere vuoto.", "Restore item path cannot be empty."))
        if not os.path.exists(source_item):
            raise FileNotFoundError(self.message("Elemento restore non trovato.", "Restore item not found."))
        destination = safe_join(self.config.source, relative_path)
        stat = os.stat(source_item)
        return {
            "name": os.path.basename(source_item),
            "path": relative_to_base(self.config.restore, source_item),
            "is_dir": os.path.isdir(source_item),
            "size": None if os.path.isdir(source_item) else stat.st_size,
            "size_human": "-" if os.path.isdir(source_item) else bytes_to_human(stat.st_size),
            "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="seconds"),
            "destination": destination,
            "destination_exists": os.path.exists(destination),
        }

    def list_restore(
        self,
        relative_path="",
        search="",
        page=1,
        page_size=DEFAULT_RESTORE_PAGE_SIZE,
        sort_by="name",
        direction="asc",
    ):
        restore_root = self.config.restore
        target = safe_join(restore_root, relative_path)
        if not os.path.exists(target):
            raise FileNotFoundError(self.message("Percorso restore non trovato.", "Restore path not found."))
        if not os.path.isdir(target):
            stat = os.stat(target)
            return {
                "path": relative_to_base(restore_root, target),
                "is_file": True,
                "size": stat.st_size,
                "size_human": bytes_to_human(stat.st_size),
                "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="seconds"),
                "items": [],
            }

        try:
            page = max(1, int(page))
            page_size = max(1, min(int(page_size), MAX_RESTORE_PAGE_SIZE))
        except (TypeError, ValueError) as exc:
            raise ValueError(self.message("Paginazione restore non valida.", "Invalid restore pagination.")) from exc
        if sort_by not in ("name", "mtime", "size"):
            raise ValueError(self.message("Campo di ordinamento restore non valido.", "Invalid restore sort field."))
        if direction not in ("asc", "desc"):
            raise ValueError(self.message("Direzione di ordinamento restore non valida.", "Invalid restore sort direction."))

        search_key = str(search or "").strip().casefold()
        items = []
        for entry in os.scandir(target):
            if search_key and search_key not in entry.name.casefold():
                continue
            try:
                stat = entry.stat()
                is_dir = entry.is_dir()
            except OSError:
                continue
            destination = safe_join(self.config.source, relative_to_base(restore_root, entry.path))
            items.append({
                "name": entry.name,
                "path": relative_to_base(restore_root, entry.path),
                "is_dir": is_dir,
                "size": None if is_dir else stat.st_size,
                "size_human": "-" if is_dir else bytes_to_human(stat.st_size),
                "mtime": datetime.fromtimestamp(stat.st_mtime).isoformat(timespec="seconds"),
                "destination_exists": os.path.exists(destination),
            })

        sort_value = {
            "name": lambda item: item["name"].casefold(),
            "mtime": lambda item: item["mtime"],
            "size": lambda item: item["size"] if item["size"] is not None else -1,
        }[sort_by]
        items.sort(key=sort_value, reverse=direction == "desc")
        items.sort(key=lambda item: not item["is_dir"])
        total = len(items)
        pages = max(1, (total + page_size - 1) // page_size)
        page = min(page, pages)
        offset = (page - 1) * page_size
        return {
            "path": relative_to_base(restore_root, target) if target != os.path.abspath(restore_root) else "",
            "is_file": False,
            "items": items[offset:offset + page_size],
            "search": search,
            "sort": sort_by,
            "direction": direction,
            "page": page,
            "page_size": page_size,
            "pages": pages,
            "total": total,
            "has_previous": page > 1,
            "has_next": page < pages,
        }

    def get_config_from_disk(self):
        return SySeBaConfig.load(self.config.config_path, self.language)

    def update_config(self, values):
        updated = self.config.save(values, self.language)
        active = self.config.as_public_dict()
        saved = updated.as_public_dict()
        self.restart_required = any(active.get(key) != saved.get(key) for key in ("source", "backup", "restore", "log_file", "threads"))
        message = self.message(
            "Configurazione aggiornata dall'interfaccia Web.",
            "Configuration updated from web interface.",
        )
        if self.restart_required:
            message += self.message(
                " Riavvia SySeBa per applicare le modifiche.",
                " Restart SySeBa to apply runtime changes.",
            )
        self.emit("INFO", message, "CONFIG", self.config.config_path)
        return updated

    def config_state(self):
        active = self.config.as_public_dict()
        saved = self.get_config_from_disk().as_public_dict()
        changes = {
            key: {"active": active.get(key), "saved": saved.get(key)}
            for key in ("source", "backup", "restore", "log_file", "threads")
            if active.get(key) != saved.get(key)
        }
        self.restart_required = bool(changes)
        return {
            "active": active,
            "saved": saved,
            "changes": changes,
            "restart_required": self.restart_required,
        }

    def service_state(self):
        systemctl = shutil.which("systemctl") if os.name != "nt" else None
        managed = bool(systemctl and os.environ.get("INVOCATION_ID"))
        return {
            "managed": managed,
            "restart_available": managed,
            "name": self.systemd_service,
            "restart_command": f"sudo systemctl restart {self.systemd_service}",
        }

    def request_service_restart(self):
        service = self.service_state()
        if not service["restart_available"]:
            raise RuntimeError(
                self.message(
                    f"Riavvio automatico non disponibile. Esegui: {service['restart_command']}",
                    f"Automatic restart is unavailable. Run: {service['restart_command']}",
                )
            )

        self.emit(
            "WARNING",
            self.message(
                f"Riavvio di {self.systemd_service} richiesto dall'interfaccia Web.",
                f"Restart requested for {self.systemd_service} from web interface.",
            ),
            "SERVICE",
        )

        def restart_after_response():
            time.sleep(0.75)
            try:
                subprocess.run(["systemctl", "--no-block", "restart", self.systemd_service], check=True, timeout=30)
            except (OSError, subprocess.SubprocessError) as exc:
                self.emit(
                    "ERROR",
                    self.message(
                        f"Impossibile riavviare {self.systemd_service}: {exc}",
                        f"Unable to restart {self.systemd_service}: {exc}",
                    ),
                    "SERVICE",
                    additional_info=str(exc),
                )

        threading.Thread(target=restart_after_response, name="syseba-service-restart", daemon=True).start()
        return service

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
        if self.initial_sync_state in ("pending", "skipped", "not_available"):
            initial_percent = None
        elif initial_total:
            initial_percent = round((initial_done / initial_total) * 100, 2)
        else:
            initial_percent = 100.0 if self.initial_sync_state.startswith("completed") else 0.0

        try:
            config_state = self.config_state()
        except (OSError, ValueError, configparser.Error):
            config_state = {
                "active": self.config.as_public_dict(),
                "saved": None,
                "changes": {},
                "restart_required": self.restart_required,
            }

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
            "last_event_at": self.last_event_at,
            "config": self.config.as_public_dict(),
            "config_state": config_state,
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
            "initial_sync": {
                "state": self.initial_sync_state,
                "percent": initial_percent,
                "total": initial_total,
                "done": initial_done,
                "started_at": self.initial_sync_started_at,
                "completed_at": self.initial_sync_completed_at,
                "error": self.initial_sync_error,
            },
            "recent_logs": recent_logs,
            "external_lock": self.external_lock_status(),
            "service": self.service_state(),
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

    def set_initial_sync_state(self, state, error=None):
        now = datetime.now().isoformat(timespec="seconds")
        with self.lock:
            self.initial_sync_state = state
            if state == "running":
                self.initial_sync_started_at = now
                self.initial_sync_completed_at = None
                self.initial_sync_error = None
            elif state in ("completed", "completed_with_errors", "stopped", "failed"):
                self.initial_sync_completed_at = now
                self.initial_sync_error = error

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

    def message(self, italian, english):
        return italian if self.daemon.language == "it" else english

    def log_message(self, format_string, *args):
        logging.info("web %s - %s", self.address_string(), format_string % args)

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)

        try:
            if path == "/":
                self.send_html(render_dashboard_html(self.daemon.language))
            elif path in ("/logo", "/favicon.ico"):
                self.send_file(os.path.join(SCRIPT_DIR, "SySeBa_Logo.webp"))
            elif path == "/webui.js":
                self.send_file(os.path.join(SCRIPT_DIR, "syseba_web.js"))
            elif path == "/api/auth":
                self.send_json({"required": bool(self.auth_token)})
            elif not self.require_auth():
                return
            elif path == "/api/status":
                self.send_json(self.daemon.status())
            elif path == "/api/logs":
                lines = int(query.get("lines", ["200"])[0])
                self.send_json({"lines": tail_file(self.daemon.config.log_file, max(1, min(lines, 2000)))})
            elif path == "/api/config":
                self.send_json(self.daemon.get_config_from_disk().as_public_dict())
            elif path == "/api/config/state":
                self.send_json(self.daemon.config_state())
            elif path == "/api/restore":
                self.send_json(self.daemon.list_restore(
                    relative_path=query.get("path", [""])[0],
                    search=query.get("search", [""])[0],
                    page=query.get("page", ["1"])[0],
                    page_size=query.get("page_size", [str(DEFAULT_RESTORE_PAGE_SIZE)])[0],
                    sort_by=query.get("sort", ["name"])[0],
                    direction=query.get("direction", ["asc"])[0],
                ))
            elif path == "/api/restore/info":
                self.send_json(self.daemon.restore_item_info(query.get("path", [""])[0]))
            elif path == "/restore/download":
                relative_path = query.get("path", [""])[0]
                target = safe_join(self.daemon.config.restore, relative_path)
                if not os.path.isfile(target):
                    self.send_error_json(
                        HTTPStatus.NOT_FOUND,
                        self.message("File non trovato.", "File not found."),
                    )
                else:
                    self.send_file(target, download=True)
            else:
                self.send_error_json(
                    HTTPStatus.NOT_FOUND,
                    self.message("Risorsa non trovata.", "Not found."),
                )
        except Exception as exc:
            self.send_exception_json(exc, HTTPStatus.INTERNAL_SERVER_ERROR)

    def do_POST(self):
        parsed = urlparse(self.path)
        try:
            if not self.require_auth():
                return
            data = self.read_json()
            if parsed.path == "/api/config":
                updated = self.daemon.update_config(data)
                if self.daemon.language == "it":
                    message = (
                        "Configurazione salvata. Riavvia SySeBa per applicarla."
                        if self.daemon.restart_required
                        else "Configurazione salvata e già attiva."
                    )
                else:
                    message = "Configuration saved. Restart SySeBa to apply it." if self.daemon.restart_required else "Configuration saved and already active."
                self.send_json({
                    "ok": True,
                    "restart_required": self.daemon.restart_required,
                    "config": updated.as_public_dict(),
                    "state": self.daemon.config_state(),
                    "message": message,
                })
            elif parsed.path == "/api/restore":
                strategy = data.get("strategy")
                restored_to = self.daemon.restore_item(
                    data.get("path", ""),
                    bool(data.get("overwrite", False)),
                    strategy=strategy,
                )
                self.send_json({
                    "ok": True,
                    "restored_to": restored_to,
                    "strategy": strategy or ("overwrite" if data.get("overwrite") else "fail"),
                    "message": "Elemento ripristinato correttamente." if self.daemon.language == "it" else "Item restored successfully.",
                })
            elif parsed.path == "/api/service/restart":
                service = self.daemon.request_service_restart()
                self.send_json({
                    "ok": True,
                    "service": service,
                    "message": "Riavvio del servizio richiesto." if self.daemon.language == "it" else "Service restart requested.",
                })
            else:
                self.send_error_json(
                    HTTPStatus.NOT_FOUND,
                    self.message("Risorsa non trovata.", "Not found."),
                )
        except Exception as exc:
            self.send_exception_json(exc, HTTPStatus.BAD_REQUEST)

    def read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        if length > MAX_JSON_BODY:
            raise ValueError(
                self.message(
                    "Corpo della richiesta troppo grande.",
                    "Request body too large.",
                )
            )
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
            {
                "ok": False,
                "error": self.message(
                    "Autenticazione richiesta.",
                    "Authentication required.",
                ),
            },
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
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
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

    def send_exception_json(self, exc, default_status):
        status = default_status
        code = "request_failed"
        if isinstance(exc, FileNotFoundError):
            status = HTTPStatus.NOT_FOUND
            code = "not_found"
        elif isinstance(exc, FileExistsError):
            status = HTTPStatus.CONFLICT
            code = "destination_exists"
        elif isinstance(exc, PermissionError):
            status = HTTPStatus.FORBIDDEN
            code = "permission_denied"
        elif isinstance(exc, (ValueError, json.JSONDecodeError)):
            status = HTTPStatus.BAD_REQUEST
            code = "invalid_request"
        error = str(exc)
        if error == "Invalid path.":
            error = self.message("Percorso non valido.", "Invalid path.")
        self.send_json({"ok": False, "code": code, "error": error}, status)

    def send_file(self, path, download=False):
        if not os.path.exists(path):
            self.send_error_json(
                HTTPStatus.NOT_FOUND,
                self.message("File non trovato.", "File not found."),
            )
            return
        mime_type = mimetypes.guess_type(path)[0] or "application/octet-stream"
        with open(path, "rb") as file_handle:
            size = os.fstat(file_handle.fileno()).st_size
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", mime_type)
            self.send_header("Content-Length", str(size))
            self.send_security_headers()
            if download:
                filename = safe_download_name(path)
                quoted = quote(filename)
                self.send_header("Content-Disposition", f'attachment; filename="{filename}"; filename*=UTF-8\'\'{quoted}')
            self.end_headers()
            while True:
                chunk = file_handle.read(1024 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)


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


def render_dashboard_html(language="it"):
    html_body = """<!doctype html>
<html lang="__LANG__">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SySeBa</title>
  <style>
    :root {
      color-scheme: light;
      --bg: #f3f5f4;
      --surface: #ffffff;
      --surface-strong: #232826;
      --text: #1b1f1d;
      --muted: #66706b;
      --line: #d5dbd8;
      --accent: #27745f;
      --accent-2: #8a3f24;
      --danger: #a61b12;
      --warn: #8b5a00;
      --ok: #176b45;
      --ok-dark: #71e2ae;
      --danger-dark: #ff938c;
      --neutral-dark: #f1d27a;
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
    .tabs {
      display: flex;
      gap: 8px;
      border-bottom: 1px solid var(--line);
      margin-bottom: 18px;
      overflow-x: auto;
    }
    .tabs button {
      border: 0;
      border-bottom: 3px solid transparent;
      background: transparent;
      padding: 12px 12px 10px;
      font: inherit;
      color: var(--muted);
      cursor: pointer;
    }
    .tabs button.active { color: var(--text); border-color: var(--accent); font-weight: 700; }
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
    .metric-label { font-size: 12px; color: var(--muted); text-transform: uppercase; letter-spacing: 0; }
    .metric-value { margin-top: 6px; font-size: 28px; font-weight: 800; line-height: 1; }
    .metric-small { margin-top: 6px; color: var(--muted); font-size: 13px; overflow-wrap: anywhere; }
    .bar { height: 9px; background: #e8e2d7; border-radius: 999px; overflow: hidden; margin-top: 10px; }
    .bar span { display: block; height: 100%; width: 0; background: var(--accent); }
    .bar.warn span { background: var(--warn); }
    .bar.danger span { background: var(--danger); }
    table { width: 100%; border-collapse: collapse; }
    th, td { padding: 10px 8px; text-align: left; border-bottom: 1px solid var(--line); font-size: 14px; }
    th { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0; }
    code, pre {
      font-family: "Cascadia Mono", "SFMono-Regular", Consolas, monospace;
      background: var(--code);
      color: #e8fff4;
      border-radius: 6px;
    }
    pre { padding: 14px; overflow: auto; max-height: 520px; line-height: 1.45; }
    label { display: grid; gap: 6px; font-size: 13px; color: var(--muted); }
    input, select {
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
    button.primary, button.secondary, button.danger {
      border: 0;
      border-radius: 6px;
      padding: 10px 13px;
      font: inherit;
      font-weight: 700;
      cursor: pointer;
    }
    button.primary { color: white; background: var(--accent); }
    button.secondary { color: var(--text); background: #e7e0d5; }
    button.danger { color: white; background: var(--danger); }
    button:disabled { cursor: not-allowed; opacity: .62; }
    button:focus-visible, input:focus-visible, select:focus-visible, [tabindex]:focus-visible {
      outline: 3px solid #d29335;
      outline-offset: 2px;
    }
    .notice { margin-top: 10px; color: var(--accent-2); font-weight: 700; }
    .hide { display: none !important; }
    .path { overflow-wrap: anywhere; }
    .status-ok { color: var(--ok); font-weight: 800; }
    .pill .status-ok { color: var(--ok-dark); }
    .status-bad { color: var(--danger); font-weight: 800; }
    .pill .status-bad { color: var(--danger-dark); }
    .status-neutral { color: var(--neutral-dark); font-weight: 800; }
    .restore-row button { padding: 6px 9px; border-radius: 6px; border: 1px solid var(--line); background: white; cursor: pointer; }
    .section-toolbar { display: flex; align-items: center; justify-content: space-between; gap: 12px; margin-bottom: 14px; flex-wrap: wrap; }
    .section-toolbar h2 { margin: 0; }
    .freshness { color: var(--muted); font-size: 13px; }
    .restart-banner, .inline-banner {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 14px;
      padding: 12px 14px;
      margin-bottom: 14px;
      border: 1px solid #d9b66d;
      border-left: 4px solid var(--warn);
      border-radius: 6px;
      background: #fff8e8;
    }
    .restart-banner strong, .inline-banner strong { display: block; margin-bottom: 3px; }
    .restart-command { color: var(--muted); font-size: 12px; overflow-wrap: anywhere; }
    .stats-grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 0 20px; margin: 0; }
    .stats-grid div { display: flex; justify-content: space-between; gap: 12px; padding: 10px 8px; border-bottom: 1px solid var(--line); }
    .stats-grid dt { color: var(--muted); font-size: 12px; font-weight: 700; text-transform: uppercase; }
    .stats-grid dd { margin: 0; font-variant-numeric: tabular-nums; }
    .control-grid { display: grid; grid-template-columns: minmax(150px, 1fr) minmax(150px, 1fr) auto auto; gap: 10px; align-items: end; }
    .compact-label { min-width: 120px; }
    .check-label { display: inline-flex; grid-template-columns: none; flex-direction: row; align-items: center; gap: 7px; padding: 10px 0; color: var(--text); }
    .check-label input { width: auto; }
    .log-view {
      min-height: 260px;
      max-height: 540px;
      overflow: auto;
      padding: 10px 0;
      border-radius: 6px;
      background: var(--code);
      color: #e8fff4;
      font: 13px/1.5 "Cascadia Mono", "SFMono-Regular", Consolas, monospace;
    }
    .log-line { padding: 2px 12px; white-space: pre-wrap; overflow-wrap: anywhere; border-left: 3px solid transparent; }
    .log-line.warning { color: #ffe19a; border-color: #d9a62e; }
    .log-line.error, .log-line.critical { color: #ffaaa4; border-color: #ff6f66; }
    .log-line.info { color: #d7f5e7; }
    .empty-state { padding: 22px; color: var(--muted); text-align: center; }
    .config-diff { margin-bottom: 14px; }
    .config-diff table { margin-top: 8px; }
    .changed-value { color: var(--accent-2); font-weight: 700; overflow-wrap: anywhere; }
    .restore-controls { display: grid; grid-template-columns: minmax(180px, 1fr) 160px 130px 110px auto; gap: 10px; align-items: end; }
    .breadcrumbs { display: flex; align-items: center; gap: 3px; flex-wrap: wrap; min-width: 0; }
    .breadcrumbs button { border: 0; background: transparent; color: var(--accent); padding: 5px; cursor: pointer; font: inherit; }
    .breadcrumbs span { color: var(--muted); }
    .table-scroll { width: 100%; overflow-x: auto; }
    .restore-row .conflict { color: var(--warn); font-weight: 700; font-size: 12px; }
    .row-actions { display: flex; gap: 6px; flex-wrap: wrap; }
    .pagination { display: flex; align-items: center; justify-content: flex-end; gap: 10px; margin-top: 12px; }
    .pagination-info { min-width: 150px; text-align: center; color: var(--muted); font-size: 13px; }
    dialog { width: min(560px, calc(100% - 32px)); border: 1px solid var(--line); border-radius: 8px; padding: 0; color: var(--text); }
    dialog::backdrop { background: rgba(16, 22, 21, .55); }
    .dialog-body { padding: 20px; }
    .dialog-body h2 { margin: 0 0 12px; font-size: 19px; }
    .dialog-details { display: grid; gap: 8px; margin: 14px 0; padding: 12px; background: var(--bg); border-radius: 6px; }
    .dialog-details div { overflow-wrap: anywhere; }
    .dialog-actions { display: flex; justify-content: flex-end; gap: 8px; flex-wrap: wrap; padding: 14px 20px; border-top: 1px solid var(--line); }
    .toast-region { position: fixed; right: 18px; bottom: 18px; z-index: 20; display: grid; gap: 8px; width: min(420px, calc(100% - 36px)); }
    .toast { padding: 12px 14px; border-radius: 6px; color: white; background: #26322e; box-shadow: 0 8px 24px rgba(0,0,0,.22); }
    .toast.error { background: #7f1d1d; }
    .toast.success { background: #145c3b; }
    .visually-hidden { position: absolute; width: 1px; height: 1px; padding: 0; margin: -1px; overflow: hidden; clip: rect(0,0,0,0); white-space: nowrap; border: 0; }
    @media (max-width: 1000px) {
      .grid.cols-4 { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .grid.cols-3, .form-grid { grid-template-columns: 1fr; }
      .stats-grid { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .control-grid, .restore-controls { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      header { align-items: flex-start; flex-direction: column; }
    }
    @media (max-width: 700px) {
      header { padding: 14px 18px; }
      main { padding: 16px; }
      .auth-panel { padding: 12px 16px; }
      .grid.cols-4, .stats-grid, .control-grid, .restore-controls { grid-template-columns: 1fr; }
      .metric-value { font-size: 24px; }
      .restart-banner, .inline-banner { align-items: flex-start; flex-direction: column; }
      .restore-table thead { display: none; }
      .restore-table, .restore-table tbody, .restore-table tr, .restore-table td { display: block; width: 100%; }
      .restore-table tr { padding: 10px 0; border-bottom: 1px solid var(--line); }
      .restore-table td { display: grid; grid-template-columns: minmax(88px, 34%) minmax(0, 1fr); gap: 8px; padding: 5px 4px; border: 0; overflow-wrap: anywhere; }
      .restore-table td::before { content: attr(data-label); color: var(--muted); font-size: 11px; font-weight: 700; text-transform: uppercase; }
      .restore-table td.actions-cell { display: block; padding-top: 9px; }
      .restore-table td.actions-cell::before { content: none; }
      .row-actions button { flex: 1 1 auto; }
      .pagination { justify-content: space-between; }
      .pagination-info { min-width: 0; }
      .toast-region { right: 12px; bottom: 12px; width: calc(100% - 24px); }
    }
  </style>
</head>
<body data-language="__LANG__">
  <header>
    <div class="brand">
      <img src="/logo" alt="SySeBa">
      <div>
        <h1>SySeBa</h1>
        <div class="subtitle">Syncro Service Backup</div>
      </div>
    </div>
    <div class="pill" id="runtime-pill" role="status" aria-live="polite" data-i18n="loading_status">Caricamento stato...</div>
  </header>
  <section class="auth-panel hide" id="auth-panel" aria-labelledby="auth-title">
    <form class="auth-inner" id="auth-form">
      <label id="auth-title"><span data-i18n="web_token">Token web SySeBa</span><input id="auth-token" type="password" autocomplete="current-password" required></label>
      <button class="primary" id="auth-save" type="submit" data-i18n="sign_in">Accedi</button>
      <button class="secondary" id="auth-clear" type="button" data-i18n="forget_token">Dimentica token</button>
      <div class="notice" id="auth-notice" role="alert"></div>
    </form>
  </section>
  <main>
    <nav class="tabs" role="tablist" aria-label="SySeBa">
      <button class="active" id="tab-button-status" role="tab" aria-selected="true" aria-controls="tab-status" data-tab="status" data-i18n="status">Stato</button>
      <button id="tab-button-logs" role="tab" aria-selected="false" aria-controls="tab-logs" data-tab="logs" data-i18n="logs">Log</button>
      <button id="tab-button-config" role="tab" aria-selected="false" aria-controls="tab-config" data-tab="config" data-i18n="configuration">Configurazione</button>
      <button id="tab-button-restore" role="tab" aria-selected="false" aria-controls="tab-restore" data-tab="restore" data-i18n="restore">Restore</button>
    </nav>

    <section id="tab-status" class="tab" role="tabpanel" aria-labelledby="tab-button-status" tabindex="0">
      <div class="section-toolbar">
        <h2 data-i18n="overview">Panoramica</h2>
        <div class="freshness" id="last-updated"></div>
      </div>
      <div class="restart-banner hide" id="restart-banner">
        <div>
          <strong data-i18n="restart_required">Riavvio necessario</strong>
          <span data-i18n="restart_explanation">La configurazione salvata non è ancora attiva.</span>
          <div class="restart-command" id="restart-command"></div>
        </div>
        <button class="primary hide" type="button" id="restart-service" data-i18n="restart_service">Riavvia servizio</button>
      </div>
      <div class="grid cols-4" id="metrics"></div>
      <div class="grid cols-3" style="margin-top:14px" id="disk"></div>
      <div class="panel" style="margin-top:14px">
        <div class="section-toolbar">
          <h2 data-i18n="session_activity">Attività dalla partenza</h2>
          <div class="freshness" id="session-started"></div>
        </div>
        <dl class="stats-grid" id="activity"></dl>
      </div>
    </section>

    <section id="tab-logs" class="tab hide" role="tabpanel" aria-labelledby="tab-button-logs" tabindex="0" hidden>
      <div class="panel">
        <div class="section-toolbar">
          <h2 data-i18n="logs">Log</h2>
          <div class="freshness" id="logs-meta"></div>
        </div>
        <div class="control-grid">
          <label class="compact-label"><span data-i18n="rows">Righe</span>
            <select id="log-lines"><option>100</option><option selected>250</option><option>500</option><option>1000</option><option>2000</option></select>
          </label>
          <label><span data-i18n="search">Cerca</span><input id="log-search" type="search" data-i18n-placeholder="search_logs" placeholder="Testo nel log"></label>
          <label class="compact-label"><span data-i18n="level">Livello</span>
            <select id="log-level"><option value="all" data-i18n="all">Tutti</option><option value="info">INFO</option><option value="warning">WARNING</option><option value="error">ERROR</option></select>
          </label>
          <label class="check-label"><input id="log-auto" type="checkbox" checked><span data-i18n="auto_refresh">Aggiorna automaticamente</span></label>
        </div>
        <div class="actions">
          <button class="primary" type="button" id="refresh-logs" data-i18n="refresh">Aggiorna</button>
          <button class="secondary" type="button" id="copy-logs" data-i18n="copy">Copia</button>
          <button class="secondary" type="button" id="download-logs" data-i18n="download">Download</button>
          <label class="check-label"><input id="log-follow" type="checkbox" checked><span data-i18n="follow_tail">Segui ultima riga</span></label>
        </div>
        <div id="logs" class="log-view" role="log" aria-label="Log SySeBa" aria-busy="false"></div>
      </div>
    </section>

    <section id="tab-config" class="tab hide" role="tabpanel" aria-labelledby="tab-button-config" tabindex="0" hidden>
      <div class="panel">
        <h2 data-i18n="configuration">Configurazione</h2>
        <div class="config-diff" id="config-diff"></div>
        <form id="config-form">
          <div class="form-grid">
            <label><span data-i18n="source">Source</span><input name="source" required></label>
            <label><span data-i18n="backup">Backup</span><input name="backup" required></label>
            <label><span data-i18n="restore">Restore</span><input name="restore" required></label>
            <label><span data-i18n="log_file">Log</span><input name="log_file" required></label>
            <label><span data-i18n="workers">Thread worker</span><input name="threads" type="number" min="1" max="64" required></label>
          </div>
          <div class="actions">
            <button class="primary" type="submit" id="save-config" data-i18n="save_configuration">Salva configurazione</button>
            <button class="secondary" type="button" id="reload-config" data-i18n="reload">Ricarica</button>
          </div>
        </form>
        <div class="notice" id="config-notice" role="status" aria-live="polite"></div>
      </div>
    </section>

    <section id="tab-restore" class="tab hide" role="tabpanel" aria-labelledby="tab-button-restore" tabindex="0" hidden>
      <div class="panel">
        <div class="section-toolbar">
          <h2 data-i18n="restore_area">Area restore</h2>
          <div class="freshness" id="restore-total"></div>
        </div>
        <div class="actions">
          <button class="secondary" type="button" id="restore-up" data-i18n="up">Su</button>
          <div class="breadcrumbs" id="restore-breadcrumbs" aria-label="Restore"></div>
        </div>
        <form class="restore-controls" id="restore-filter">
          <label><span data-i18n="search">Cerca</span><input id="restore-search" type="search" data-i18n-placeholder="search_folder" placeholder="Cerca nella cartella"></label>
          <label><span data-i18n="sort_by">Ordina per</span><select id="restore-sort"><option value="name" data-i18n="name">Nome</option><option value="mtime" data-i18n="modified">Modifica</option><option value="size" data-i18n="size">Dimensione</option></select></label>
          <label><span data-i18n="direction">Direzione</span><select id="restore-direction"><option value="asc" data-i18n="ascending">Crescente</option><option value="desc" data-i18n="descending">Decrescente</option></select></label>
          <label><span data-i18n="per_page">Per pagina</span><select id="restore-page-size"><option>25</option><option selected>100</option><option>250</option></select></label>
          <button class="primary" type="submit" data-i18n="apply">Applica</button>
        </form>
        <div class="table-scroll">
          <table class="restore-table" style="margin-top:12px">
            <thead><tr><th scope="col" data-i18n="name">Nome</th><th scope="col" data-i18n="type">Tipo</th><th scope="col" data-i18n="size">Dimensione</th><th scope="col" data-i18n="modified">Modifica</th><th scope="col" data-i18n="actions">Azioni</th></tr></thead>
            <tbody id="restore-list"></tbody>
          </table>
        </div>
        <div class="pagination">
          <button class="secondary" type="button" id="restore-prev" data-i18n="previous">Precedente</button>
          <div class="pagination-info" id="restore-page-info"></div>
          <button class="secondary" type="button" id="restore-next" data-i18n="next">Successiva</button>
        </div>
      </div>
    </section>
  </main>
  <dialog id="restore-dialog" aria-labelledby="restore-dialog-title">
    <div class="dialog-body">
      <h2 id="restore-dialog-title" data-i18n="confirm_restore">Conferma ripristino</h2>
      <div id="restore-dialog-message"></div>
      <div class="dialog-details" id="restore-dialog-details"></div>
    </div>
    <div class="dialog-actions">
      <button class="secondary" type="button" id="restore-cancel" data-i18n="cancel">Annulla</button>
      <button class="secondary hide" type="button" id="restore-rename" data-strategy="rename" data-i18n="restore_rename">Ripristina con nuovo nome</button>
      <button class="danger hide" type="button" id="restore-overwrite" data-strategy="overwrite" data-i18n="restore_overwrite">Sovrascrivi o unisci</button>
      <button class="primary" type="button" id="restore-confirm" data-strategy="fail" data-i18n="restore_now">Ripristina</button>
    </div>
  </dialog>
  <div class="toast-region" id="toast-region" aria-live="polite" aria-atomic="true"></div>
  <script src="/webui.js" defer></script>
</body>
</html>
"""
    return html_body.replace("__LANG__", language if language in ("it", "en") else "it")


class ConsoleDashboard:
    def __init__(self, daemon, refresh_seconds=3):
        self.daemon = daemon
        self.refresh_seconds = refresh_seconds

    def run(self):
        try:
            if not sys.stdout.isatty():
                self.render()
                while not self.daemon.stop_event.is_set():
                    self.daemon.stop_event.wait(1)
                return
            while not self.daemon.stop_event.is_set():
                self.render()
                self.daemon.stop_event.wait(self.refresh_seconds)
        except KeyboardInterrupt:
            self.daemon.stop_event.set()

    def render(self):
        status = self.daemon.status()
        terminal = shutil.get_terminal_size((100, 30))
        width = max(40, terminal.columns)
        height = max(12, terminal.lines)
        if sys.stdout.isatty():
            print("\033[2J\033[H", end="")
        section_titles = {
            self.label("PATHS", "Paths"),
            self.label("PROCESS", "Process"),
            self.label("LOG", "Log"),
            self.label("RECENT_EVENTS", "Recent events"),
        }
        for line in self.build_lines(status, width, height):
            stripped = line.strip()
            if set(stripped) == {"="}:
                print(color(line, "cyan"))
            elif stripped in section_titles:
                print(color(line, "cyan"))
            elif stripped == self.label("CONFIG_RESTART", "Configuration saved: restart required."):
                print(color(line, "yellow"))
            elif stripped == self.label("CLEAN_EXIT", "Ctrl+C to exit cleanly."):
                print(color(line, "cyan"))
            else:
                print(line)

    def build_lines(self, status, width, height):
        width = max(40, int(width))
        height = max(12, int(height))
        compact = width < 72 or height < 24
        separator = "=" * min(width, 100)
        lines = [separator, truncate(" SySeBa | The Syncro Service Backup", width), separator]

        runtime = self.label("RUNNING", "RUNNING") if status["running"] else self.label("WEB_ONLY", "WEB ONLY")
        runtime_fields = [
            f"{self.label('STATUS', 'Status')}: {runtime}",
            f"PID: {status['pid']}",
            f"Uptime: {status['uptime']}",
            f"{self.label('CLOCK', 'Time')}: {datetime.now().strftime('%H:%M:%S')}",
        ]
        lines.extend(self.pack_fields(runtime_fields, width))
        if status["restart_required"]:
            lines.append(truncate(" " + self.label("CONFIG_RESTART", "Configuration saved: restart required."), width))

        self.append_gap(lines, compact)
        lines.append(" " + self.label("PATHS", "Paths"))
        lines.extend(self.format_disk("SOURCE", status["disk"]["source"], width))
        lines.extend(self.format_disk("BACKUP", status["disk"]["backup"], width))
        lines.extend(self.format_disk("RESTORE", status["disk"]["restore"], width))

        self.append_gap(lines, compact)
        lines.append(" " + self.label("PROCESS", "Process"))
        process = status["process"]
        process_fields = [
            f"CPU: {value_or_na(process['cpu_percent'], '%')}",
            f"RAM: {value_or_na(process['memory_mb'], ' MB')}",
            f"{self.label('THREADS', 'Threads')}: {process['threads']}",
            f"{self.label('QUEUE', 'Queue')}: {process['queue_size']}",
        ]
        lines.extend(self.pack_fields(process_fields, width))

        stats = status["stats"]
        stat_fields = [
            f"{self.label('COPIED', 'Copied')}: {stats['copied']}",
            f"{self.label('UPDATED', 'Updated')}: {stats['updated']}",
            f"{self.label('DELETED', 'In restore')}: {stats['deleted']}",
            f"{self.label('RESTORED', 'Restored')}: {stats['restored']}",
            f"{self.label('ERRORS', 'Errors')}: {stats['errors']}",
        ]
        lines.extend(self.pack_fields(stat_fields, width))

        sync = status.get("initial_sync") or {
            "state": "running" if stats.get("initial_running") else "pending",
            "percent": status.get("initial_sync_percent"),
            "done": stats.get("initial_done", 0),
            "total": stats.get("initial_total", 0),
        }
        state_key = "SYNC_" + str(sync.get("state", "pending")).upper()
        state_label = self.label(state_key, str(sync.get("state", "pending")))
        sync_parts = [f"{self.label('INITIAL_SYNC', 'Initial sync')}: {state_label}"]
        if sync.get("percent") is not None:
            sync_parts.append(f"{float(sync['percent']):.2f}%")
        if sync.get("total"):
            sync_parts.append(f"{sync.get('done', 0)}/{sync['total']}")
        lines.extend(self.pack_fields(sync_parts, width))

        self.append_gap(lines, compact)
        lines.append(" " + self.label("LOG", "Log"))
        lines.append(" " + truncate(status["config"]["log_file"], width - 1))

        footer = " " + self.label("CLEAN_EXIT", "Ctrl+C to exit cleanly.")
        remaining = height - len(lines) - 1
        if remaining >= 2:
            self.append_gap(lines, compact)
            if len(lines) < height - 1:
                lines.append(" " + self.label("RECENT_EVENTS", "Recent events"))
            available_events = max(0, height - len(lines) - 1)
            recent = status.get("recent_logs", [])[-min(8, available_events):]
            if recent:
                lines.extend(" " + truncate(entry, width - 1) for entry in recent)
            elif available_events:
                lines.append(" " + self.label("NO_EVENTS", "No recent events."))

        lines = lines[:height - 1]
        lines.append(truncate(footer, width))
        return [truncate(line, width) for line in lines]

    def format_disk(self, label_key, item, width):
        label = self.label(label_key, label_key)
        if not item["exists"]:
            return [truncate(f" {label}: {item['path']} | {self.label('NOT_FOUND', 'not found')}", width)]
        percent = float(item["used_percent"] or 0)
        if width < 72:
            prefix = f" {label}: {percent:.2f}% "
            return [prefix + truncate(item["path"], max(1, width - len(prefix)))]
        bar_width = min(24, max(10, width // 6))
        suffix = f" | {plain_bar(percent, bar_width)} {percent:6.2f}%"
        prefix = f" {label:<7}: "
        path_width = max(8, width - len(prefix) - len(suffix))
        return [truncate(prefix + truncate(item["path"], path_width) + suffix, width)]

    @staticmethod
    def pack_fields(fields, width):
        lines = []
        current = " "
        for field in fields:
            candidate = field if current == " " else " | " + field
            if len(current) + len(candidate) <= width:
                current += candidate
            else:
                if current.strip():
                    lines.append(truncate(current, width))
                current = " " + field
        if current.strip():
            lines.append(truncate(current, width))
        return lines

    @staticmethod
    def append_gap(lines, compact):
        if not compact:
            lines.append("")

    def label(self, key, fallback):
        return self.daemon.lang.get(key, fallback)


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
    if os.environ.get("NO_COLOR") is not None or not getattr(sys.stdout, "isatty", lambda: False)():
        return str(text)
    colors = {
        "red": "\033[91m",
        "green": "\033[92m",
        "yellow": "\033[93m",
        "cyan": "\033[96m",
        "reset": "\033[0m",
    }
    return colors.get(name, "") + str(text) + colors["reset"]


def colored_bar(percent, width):
    raw = plain_bar(percent, width)
    if percent >= 90:
        return color(raw, "red")
    if percent >= 75:
        return color(raw, "yellow")
    return color(raw, "green")


def plain_bar(percent, width):
    safe_percent = max(0.0, min(float(percent or 0), 100.0))
    filled = int((safe_percent / 100) * width)
    return "#" * filled + "-" * (width - filled)


def start_web_server(daemon, host, port, auth_token=None, auth_source="disabled"):
    SySeBaHTTPHandler.daemon_ref = daemon
    SySeBaHTTPHandler.auth_token = auth_token
    SySeBaHTTPHandler.auth_source = auth_source
    server = ThreadingHTTPServer((host, port), SySeBaHTTPHandler)
    thread = threading.Thread(target=server.serve_forever, name="syseba-web", daemon=True)
    thread.start()
    daemon.emit(
        "INFO",
        daemon.message(
            f"Interfaccia Web in ascolto su http://{host}:{port}",
            f"Web interface listening on http://{host}:{port}",
        ),
        "WEB",
    )
    if auth_token:
        daemon.emit(
            "INFO",
            daemon.message(
                f"Autenticazione Web attiva (token: {auth_source}).",
                f"Web authentication enabled ({auth_source} token).",
            ),
            "WEB",
        )
    else:
        daemon.emit(
            "WARNING",
            daemon.message(
                "Autenticazione Web disabilitata su richiesta esplicita.",
                "Web authentication disabled by explicit request.",
            ),
            "WEB",
        )
    return server


def create_systemd_service(
    config_path=None,
    web_host=DEFAULT_SERVICE_WEB_HOST,
    web_port=DEFAULT_WEB_PORT,
    web_token_file=DEFAULT_WEB_TOKEN_FILE,
    no_web_auth=False,
    language="it",
    install_dir=DEFAULT_INSTALL_DIR,
    unit_path=DEFAULT_SYSTEMD_UNIT,
    python_executable="/usr/bin/python3",
    systemctl_executable="systemctl",
):
    install_dir = os.path.abspath(install_dir)
    unit_path = os.path.abspath(unit_path)
    token_path = os.path.abspath(web_token_file or DEFAULT_WEB_TOKEN_FILE)
    command = [
        python_executable,
        os.path.join(install_dir, "syseba.py"),
        "--silent",
        "--web",
        "--web-host",
        web_host,
        "--web-port",
        str(web_port),
        "--lang",
        language,
    ]
    if config_path:
        command.extend(["--config", config_path])
    token_created = False
    if no_web_auth:
        command.append("--no-web-auth")
    else:
        _, token_path, token_created = ensure_web_token_file(token_path, language)
        command.extend(["--web-token-file", token_path])
    exec_start = " ".join(shlex.quote(part) for part in command)
    service_content = f"""[Unit]
Description={APP_TITLE}
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
ExecStart={exec_start}
WorkingDirectory={install_dir}
Environment=PYTHONUNBUFFERED=1
Restart=always
RestartSec=5
TimeoutStopSec=45
User=root
Group=root
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=full
ProtectHome=read-only
ReadWritePaths={install_dir} /var/log -/storage -/backup -/restore -/dati -/mnt -/media -/srv
UMask=0077

[Install]
WantedBy=multi-user.target
"""

    atomic_write_text(unit_path, service_content, mode=0o644)
    service_name = os.path.basename(unit_path)
    subprocess.run([systemctl_executable, "daemon-reload"], check=True)
    subprocess.run([systemctl_executable, "enable", service_name], check=True)
    if language == "it":
        print(f"Servizio systemd creato e abilitato: {service_name}")
        print(f"Web UI LAN: http://<IP-del-server>:{web_port}")
        if no_web_auth:
            print("ATTENZIONE: autenticazione Web disabilitata.")
        else:
            action = "creato" if token_created else "riutilizzato"
            print(f"Token Web {action}: {token_path}")
    else:
        print(f"Systemd service created and enabled: {service_name}")
        print(f"LAN Web UI: http://<server-IP>:{web_port}")
        if no_web_auth:
            print("WARNING: Web authentication is disabled.")
        else:
            action = "created" if token_created else "reused"
            print(f"Web token {action}: {token_path}")
    return {
        "service": service_name,
        "unit_path": unit_path,
        "web_host": web_host,
        "web_port": web_port,
        "token_path": None if no_web_auth else token_path,
        "token_created": token_created,
    }


def valid_port(value):
    port = int(value)
    if port < 1 or port > 65535:
        raise argparse.ArgumentTypeError("port must be between 1 and 65535")
    return port


def positive_float(value):
    number = float(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("value must be greater than zero")
    return number


def positive_int(value):
    number = int(value)
    if number <= 0:
        raise argparse.ArgumentTypeError("value must be greater than zero")
    return number


def build_parser(language="it"):
    italian = language == "it"
    help_text = {
        "command": "Comando operativo" if italian else "Operational command",
        "daemon": (
            "Crea e abilita il servizio systemd con Web UI automatica"
            if italian
            else "Create and enable the systemd service with automatic Web UI"
        ),
        "silent": "Avvia senza dashboard console" if italian else "Run without the console dashboard",
        "config": "Percorso di un file di configurazione alternativo" if italian else "Alternate configuration file path",
        "lang": "Lingua di CLI, console e Web UI" if italian else "Language for CLI, console, and Web UI",
        "web": "Avvia la Web UI insieme al watcher" if italian else "Start the Web UI with the watcher",
        "web_only": "Avvia solo la Web UI, senza watcher" if italian else "Start only the Web UI, without the watcher",
        "host": (
            "Indirizzo Web; manuale: 127.0.0.1, servizio: 0.0.0.0"
            if italian
            else "Web address; manual default: 127.0.0.1, service default: 0.0.0.0"
        ),
        "port": "Porta Web (predefinita: 8765)" if italian else "Web port (default: 8765)",
        "token": "Token richiesto dalla Web UI" if italian else "Token required by the Web UI",
        "token_file": (
            "File persistente del token Web (creato automaticamente con permessi 0600)"
            if italian
            else "Persistent Web token file (created automatically with mode 0600)"
        ),
        "no_auth": "Disabilita autenticazione Web (non sicuro)" if italian else "Disable Web authentication (unsafe)",
        "no_sync": "Salta la sincronizzazione iniziale" if italian else "Skip initial synchronization",
        "lock": "Percorso del lock file" if italian else "Lock file path",
        "db": "Percorso del database SQLite" if italian else "SQLite database path",
        "refresh": "Intervallo di refresh console in secondi" if italian else "Console refresh interval in seconds",
    }
    parser = argparse.ArgumentParser(description=APP_TITLE)
    parser.add_argument(
        "command",
        nargs="?",
        default="run",
        choices=["run", "status", "logs", "config-check", "restore-list", "restore-copy", "service-install"],
        help=help_text["command"],
    )
    parser.add_argument("--create-daemon", action="store_true", help=help_text["daemon"])
    parser.add_argument("--silent", action="store_true", help=help_text["silent"])
    parser.add_argument("--config", help=help_text["config"])
    parser.add_argument("--lang", choices=["it", "en"], default=language, help=help_text["lang"])
    parser.add_argument("--web", action="store_true", help=help_text["web"])
    parser.add_argument("--web-only", action="store_true", help=help_text["web_only"])
    parser.add_argument("--web-host", help=help_text["host"])
    parser.add_argument("--web-port", type=valid_port, default=DEFAULT_WEB_PORT, help=help_text["port"])
    parser.add_argument("--web-token", help=help_text["token"])
    parser.add_argument("--web-token-file", help=help_text["token_file"])
    parser.add_argument("--no-web-auth", action="store_true", help=help_text["no_auth"])
    parser.add_argument("--no-initial-sync", action="store_true", help=help_text["no_sync"])
    parser.add_argument("--lockfile", default=DEFAULT_LOCKFILE, help=help_text["lock"])
    parser.add_argument("--db-path", default=DEFAULT_DB_PATH, help=help_text["db"])
    parser.add_argument("--console-refresh", type=positive_float, default=3.0, help=help_text["refresh"])
    parser.add_argument("--lines", type=positive_int, default=100, help="Numero di righe log" if italian else "Number of log lines")
    parser.add_argument("--path", default="", help="Percorso relativo nell'area restore" if italian else "Relative path in the restore area")
    parser.add_argument("--search", default="", help="Filtro nome per restore-list" if italian else "Name filter for restore-list")
    parser.add_argument("--page", type=positive_int, default=1, help="Pagina risultati" if italian else "Results page")
    parser.add_argument("--page-size", type=positive_int, default=DEFAULT_RESTORE_PAGE_SIZE, help="Elementi per pagina" if italian else "Items per page")
    parser.add_argument("--sort", choices=["name", "mtime", "size"], default="name", help="Ordinamento restore" if italian else "Restore sort field")
    parser.add_argument("--direction", choices=["asc", "desc"], default="asc", help="Direzione ordinamento" if italian else "Sort direction")
    strategy = parser.add_mutually_exclusive_group()
    strategy.add_argument("--overwrite", action="store_true", help="Sovrascrivi o unisci la destinazione" if italian else "Overwrite or merge the destination")
    strategy.add_argument("--rename", action="store_true", help="Ripristina con un nuovo nome" if italian else "Restore with a new name")
    parser.add_argument("--json", dest="output_json", action="store_true", help="Output JSON")
    return parser


def validate_config(config, language="en"):
    italian = language == "it"
    errors = []
    warnings = []
    paths = {
        "source": os.path.realpath(config.source),
        "backup": os.path.realpath(config.backup),
        "restore": os.path.realpath(config.restore),
    }
    if not os.path.isdir(config.source):
        errors.append(
            (
                f"la sorgente non esiste o non è una directory: {config.source}"
                if italian
                else f"source does not exist or is not a directory: {config.source}"
            )
        )
    if len(set(paths.values())) != len(paths):
        errors.append(
            "sorgente, backup e restore devono essere directory diverse"
            if italian
            else "source, backup and restore must be different directories"
        )
    path_names = list(paths)
    for index, first_name in enumerate(path_names):
        for second_name in path_names[index + 1:]:
            first = paths[first_name]
            second = paths[second_name]
            if first == second:
                continue
            try:
                common = os.path.commonpath([first, second])
                if common in (first, second):
                    errors.append(
                        (
                            f"{first_name} e {second_name} non possono contenersi a vicenda"
                            if italian
                            else f"{first_name} and {second_name} cannot contain one another"
                        )
                    )
            except ValueError:
                pass
    try:
        log_path = os.path.realpath(config.log_file)
        if os.path.commonpath([paths["source"], log_path]) == paths["source"]:
            errors.append(
                "il file di log non può trovarsi nella sorgente"
                if italian
                else "log file cannot be inside source"
            )
    except ValueError:
        pass
    if config.threads < 1 or config.threads > 64:
        errors.append(
            "il numero di thread deve essere compreso tra 1 e 64"
            if italian
            else "threads must be between 1 and 64"
        )
    if not os.path.exists(config.backup):
        warnings.append(
            f"la directory di backup verrà creata: {config.backup}"
            if italian
            else f"backup will be created: {config.backup}"
        )
    if not os.path.exists(config.restore):
        warnings.append(
            f"la directory di restore verrà creata: {config.restore}"
            if italian
            else f"restore will be created: {config.restore}"
        )
    return errors, warnings


def execute_cli_command(args, config, lang):
    daemon = SySeBaDaemon(
        config,
        lang=lang,
        lockfile=args.lockfile,
        db_path=args.db_path,
        no_initial_sync=args.no_initial_sync,
        language=args.lang,
    )
    if args.command == "status":
        lock = daemon.external_lock_status()
        payload = {
            "running": lock["running"],
            "pid": lock["pid"],
            "config": config.as_public_dict(),
            "disk": {name: disk_usage(getattr(config, name)) for name in ("source", "backup", "restore")},
        }
        if args.output_json:
            print(json.dumps(payload, indent=2))
        else:
            state = lang.get("RUNNING", "RUNNING") if lock["running"] else lang.get("STOPPED", "STOPPED")
            print(f"SySeBa: {state}" + (f" (PID {lock['pid']})" if lock["pid"] else ""))
            for name, item in payload["disk"].items():
                detail = f"{item['used_percent']:.2f}%" if item["exists"] else lang.get("NOT_FOUND", "not found")
                print(f"{lang.get(name.upper(), name)}: {item['path']} [{detail}]")
        return 0

    if args.command == "logs":
        lines = tail_file(config.log_file, min(args.lines, 2000))
        if args.output_json:
            print(json.dumps({"lines": lines}, indent=2))
        else:
            print("\n".join(lines))
        return 0

    if args.command == "config-check":
        errors, warnings = validate_config(config, args.lang)
        payload = {"ok": not errors, "errors": errors, "warnings": warnings, "config": config.as_public_dict()}
        if args.output_json:
            print(json.dumps(payload, indent=2))
        else:
            print("OK" if not errors else lang.get("ERROR", "ERROR"))
            for warning in warnings:
                print(f"{lang.get('WARNING', 'WARNING')}: {warning}")
            for error in errors:
                print(f"{lang.get('ERROR', 'ERROR')}: {error}")
        return 0 if not errors else 2

    if args.command == "restore-list":
        result = daemon.list_restore(args.path, args.search, args.page, args.page_size, args.sort, args.direction)
        if args.output_json:
            print(json.dumps(result, indent=2))
        elif result.get("is_file"):
            print(f"{lang.get('FILE', 'FILE')}  {result['size_human']:>10}  {result['mtime']}  {result['path']}")
        else:
            print(
                f"{result['path'] or '/'} - {result['total']} {lang.get('ITEMS', 'items')} "
                f"- {lang.get('PAGE', 'Page')} {result['page']}/{result['pages']}"
            )
            for item in result["items"]:
                kind = lang.get("DIRECTORY", "DIR") if item["is_dir"] else lang.get("FILE", "FILE")
                conflict = (
                    f" [{lang.get('DESTINATION_EXISTS', 'destination exists')}]"
                    if item["destination_exists"]
                    else ""
                )
                print(f"{kind}  {item['size_human']:>10}  {item['mtime']}  {item['path']}{conflict}")
        return 0

    if args.command == "restore-copy":
        if not args.path:
            raise ValueError(
                "restore-copy richiede --path"
                if args.lang == "it"
                else "restore-copy requires --path"
            )
        strategy = "rename" if args.rename else ("overwrite" if args.overwrite else "fail")
        daemon.start_web_only()
        try:
            destination = daemon.restore_item(args.path, strategy=strategy)
            daemon.log_queue.join()
        finally:
            daemon.stop()
        if args.output_json:
            print(json.dumps({"ok": True, "restored_to": destination, "strategy": strategy}, indent=2))
        else:
            print(destination)
        return 0

    return None


def main():
    language_parser = argparse.ArgumentParser(add_help=False)
    language_parser.add_argument("--lang", choices=["it", "en"], default="it")
    language_args, _ = language_parser.parse_known_args()
    parser = build_parser(language_args.lang)
    args = parser.parse_args()

    if args.create_daemon or args.command == "service-install":
        create_systemd_service(
            config_path=args.config,
            web_host=args.web_host or DEFAULT_SERVICE_WEB_HOST,
            web_port=args.web_port,
            web_token_file=args.web_token_file or DEFAULT_WEB_TOKEN_FILE,
            no_web_auth=args.no_web_auth,
            language=args.lang,
        )
        return

    try:
        config = SySeBaConfig.load(args.config, args.lang)
        lang = load_language(lang=args.lang)
    except Exception as exc:
        parser.exit(2, f"{'Errore' if args.lang == 'it' else 'Error'}: {exc}\n")
    if args.command != "run":
        try:
            result = execute_cli_command(args, config, lang)
        except Exception as exc:
            print(f"{'Errore' if args.lang == 'it' else 'Error'}: {exc}", file=sys.stderr)
            raise SystemExit(1) from None
        if result:
            raise SystemExit(result)
        return

    daemon = SySeBaDaemon(
        config,
        lang=lang,
        lockfile=args.lockfile,
        db_path=args.db_path,
        no_initial_sync=args.no_initial_sync,
        language=args.lang,
    )
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
            web_host = args.web_host or DEFAULT_WEB_HOST
            web_token, web_token_source = load_web_token(
                args.web_token,
                args.web_token_file,
                args.no_web_auth,
                args.lang,
            )
            web_server = start_web_server(daemon, web_host, args.web_port, web_token, web_token_source)
            if args.lang == "it":
                print(f"Interfaccia Web: http://{web_host}:{args.web_port}")
            else:
                print(f"Web interface: http://{web_host}:{args.web_port}")
            if web_token:
                if web_token_source in ("generated-file", "file"):
                    token_path = os.path.abspath(
                        args.web_token_file
                        or os.environ.get("SYSEBA_WEB_TOKEN_FILE", "").strip()
                        or DEFAULT_WEB_TOKEN_FILE
                    )
                    if args.lang == "it":
                        action = "creato" if web_token_source == "generated-file" else "caricato"
                        print(f"Token Web {action} da: {token_path}")
                    else:
                        action = "created" if web_token_source == "generated-file" else "loaded"
                        print(f"Web token {action} from: {token_path}")
                else:
                    label = "Origine token Web" if args.lang == "it" else "Web token source"
                    print(f"{label}: {web_token_source}")
            else:
                print(
                    "ATTENZIONE: autenticazione Web disabilitata."
                    if args.lang == "it"
                    else "WARNING: Web authentication is disabled."
                )

        if not args.silent and not args.web_only:
            ConsoleDashboard(daemon, args.console_refresh).run()
        else:
            while not daemon.stop_event.is_set():
                daemon.stop_event.wait(1)
    except KeyboardInterrupt:
        daemon.stop_event.set()
    except Exception as exc:
        logging.error(str(exc))
        print(f"{'Errore' if args.lang == 'it' else 'Error'}: {exc}", file=sys.stderr)
        sys.exit(1)
    finally:
        if web_server is not None:
            web_server.shutdown()
            web_server.server_close()
        daemon.stop()


if __name__ == "__main__":
    main()
