import os
import shutil
import threading
import time
import logging
import argparse
import psutil
from queue import Queue
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler
import configparser
from datetime import datetime
import sqlite3

logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

LOCKFILE_PATH = "/opt/syseba/syseba.lock"
start_time = time.time()
DB_PATH = "/opt/syseba/syseba_logs.db"

def initialize_database():
    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS logs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT,
            operation TEXT,
            source_path TEXT,
            target_path TEXT,
            additional_info TEXT
        )
    ''')
    conn.commit()
    conn.close()

def log_to_database(operation, source_path, target_path=None, additional_info=None):
    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()
    cursor.execute('''
        INSERT INTO logs (timestamp, operation, source_path, target_path, additional_info) 
        VALUES (?, ?, ?, ?, ?)
    ''', (datetime.now().strftime('%Y-%m-%d %H:%M:%S'), operation, source_path, target_path, additional_info))
    conn.commit()
    conn.close()

def find_config_file(config_path=None):
    if config_path and os.path.exists(config_path):
        return config_path
    possible_paths = ["syseba.conf", "/etc/syseba/syseba.conf", "/opt/syseba/syseba.conf"]
    for path in possible_paths:
        if os.path.exists(path):
            return path
    raise FileNotFoundError("Config file not found in standard locations.")

def load_language(lang_file="/opt/syseba/syseba.lang", lang="it"):
    lang_dict = {}
    with open(lang_file, "r") as f:
        for line in f:
            parts = line.strip().split(";")
            if len(parts) == 3:
                label, it, en = parts
                lang_dict[label] = it if lang == "it" else en
    return lang_dict

def load_config(config_path=None):
    config_path = find_config_file(config_path)
    config = configparser.ConfigParser()
    config.read(config_path)
    if not config.has_section("SETTINGS"):
        raise ValueError("Invalid or missing config file!")
    settings = config["SETTINGS"]
    source = settings.get("source", "/dati")
    backup = settings.get("backup", "/backup")
    restore = settings.get("restore", "/restore")
    log_file = settings.get("log", "/var/log/syseba.log")
    threads = settings.getint("threads", 4)

    return source, backup, restore, log_file, threads

def initial_sync(source, backup, log_queue):
    total_files = sum(len(files) for _, _, files in os.walk(source))
    synced_files = 0

    for root, dirs, files in os.walk(source):
        relative_path = os.path.relpath(root, source)
        backup_path = os.path.join(backup, relative_path)
        if not os.path.exists(backup_path):
            os.makedirs(backup_path)

        for file in files:
            src_file = os.path.join(root, file)
            dest_file = os.path.join(backup_path, file)
            if not os.path.exists(dest_file):
                shutil.copy2(src_file, dest_file)
                synced_files += 1
                log_queue.put(f"Copied: {src_file} -> {dest_file} ({synced_files}/{total_files})")
                log_to_database("COPY", src_file, dest_file)
    log_queue.put("Initial sync completed.")
    log_to_database("INFO", "Initial sync completed.")

def log_writer(log_queue, log_file):
    with open(log_file, 'a') as logf:
        while True:
            try:
                message = log_queue.get(timeout=1)
                if message == "STOP":
                    break
                logf.write(message + "\n")
                logf.flush()
            except:
                continue

class BackupHandler(FileSystemEventHandler):
    def __init__(self, source, backup, restore, queue):
        self.source = source
        self.backup = backup
        self.restore = restore
        self.queue = queue

    def on_created(self, event):
        if not event.is_directory:
            self.queue.put(("create", event.src_path))

    def on_deleted(self, event):
        if not event.is_directory:
            self.queue.put(("delete", event.src_path))

    def on_modified(self, event):
        if not event.is_directory:
            self.queue.put(("modify", event.src_path))

def worker_thread(queue, source, backup, restore, log_queue):
    while True:
        try:
            operation, src_path = queue.get()
            relative_path = os.path.relpath(src_path, source)
            backup_path = os.path.join(backup, relative_path)

            if operation in ["create", "modify"]:
                if not os.path.exists(os.path.dirname(backup_path)):
                    os.makedirs(os.path.dirname(backup_path))
                shutil.copy2(src_path, backup_path)
                log_queue.put(f"{operation.capitalize()}d: {src_path} -> {backup_path}")
                log_to_database(operation.upper(), src_path, backup_path)

            elif operation == "delete":
                if os.path.exists(backup_path):
                    restore_path = os.path.join(restore, relative_path)
                    if not os.path.exists(os.path.dirname(restore_path)):
                        os.makedirs(os.path.dirname(restore_path))
                    shutil.move(backup_path, restore_path)
                    log_queue.put(f"Deleted: {backup_path}, Moved to restore: {restore_path}")
                    log_to_database("DELETE", backup_path, restore_path, "Moved to restore")

            queue.task_done()
        except Exception as e:
            log_queue.put(f"Error processing {operation} for {src_path}: {e}")
            log_to_database("ERROR", src_path, additional_info=str(e))

def create_systemd_service():
    service_content = """[Unit]
Description=SySeBa - The Syncro Service Backup
After=network.target

[Service]
ExecStart=/usr/bin/python3 /opt/syseba/syseba.py --silent
WorkingDirectory=/opt/syseba
Restart=always
User=root
Group=root

[Install]
WantedBy=multi-user.target
"""

    with open("/etc/systemd/system/syseba.service", "w") as service_file:
        service_file.write(service_content)
    os.system("systemctl daemon-reload")
    os.system("systemctl enable syseba.service")
    print("Systemd service created and enabled.")

def is_daemon_running():
    """
    Daemon check.
    """
    if os.path.exists(LOCKFILE_PATH):
        try:
            with open(LOCKFILE_PATH, 'r') as lockfile:
                pid = int(lockfile.read().strip())
                if os.path.exists(f"/proc/{pid}"):
                    return True
        except (ValueError, OSError):
            pass

    with open(LOCKFILE_PATH, 'w') as lockfile:
        lockfile.write(str(os.getpid()))

    return False

def get_disk_usage(path):
    usage = shutil.disk_usage(path)
    return (usage.used / usage.total) * 100

def generate_ascii_bar(percentage):
    bar_length = 50
    filled_length = int(percentage / 100 * bar_length)
    if percentage <= 60:
        color = "\033[92m"  
    elif percentage <= 85:
        color = "\033[93m" 
    else:
        color = "\033[91m"  

    bar = color + "?" * filled_length + "-" * (bar_length - filled_length) + "\033[0m"
    return bar

def get_process_memory_usage():
    process = psutil.Process(os.getpid())
    return process.memory_info().rss / (1024 ** 2) 

def get_process_cpu_usage():
    process = psutil.Process(os.getpid())
    return process.cpu_percent(interval=0.1)

def display_status(source, backup, log_file, stop_event, lang):
    try:
        while not stop_event.is_set():
            source_usage = get_disk_usage(source)
            backup_usage = get_disk_usage(backup)
            memory_usage = get_process_memory_usage()
            cpu_usage = get_process_cpu_usage()

            os.system("clear")
            print("\033[91m") 
            print("""
                   SySeBa  
            The Syncro Service Backup
                   by okno
            """)
            print("\033[0m")  

            print(f"{lang['SPACE_USED']} /dati: {source_usage:.2f}%")
            print(generate_ascii_bar(source_usage))
            print(f"{lang['SPACE_USED']} /backup: {backup_usage:.2f}%")
            print(generate_ascii_bar(backup_usage))

            print(f"CPU Usage: {cpu_usage:.2f}%")
            print(generate_ascii_bar(cpu_usage))

            print("\033[92m")  
            print(f"{lang['LOG']}: {log_file}")
            print("\033[0m")

            elapsed_time = time.time() - start_time
            print(f"Elapsed Time: {elapsed_time:.2f} seconds")
            print(f"Memory Usage: {memory_usage:.2f} MB")

            current_time = datetime.now().strftime("%H:%M:%S")
            print(f"{lang['CLOCK']}: {current_time}")

            time.sleep(3)
    except KeyboardInterrupt:
        stop_event.set()

def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--help", action="store_true")
    parser.add_argument("--create-daemon", action="store_true", help="Create and enable SySeBa as a systemd service")
    parser.add_argument("--silent", action="store_true", help="Run in silent mode (log only)")
    parser.add_argument("--config", help="Specify custom config file path")
    parser.add_argument("--lang", choices=["it", "en"], default="it", help="Select language (it or en)")
    args = parser.parse_args()

    if args.help:
        print("SySeBa - The Syncro Service Backup\n\nOptions:\n  --create-daemon  Create and enable SySeBa as a systemd service\n  --silent         Run in silent mode (log only)\n  --config         Specify custom config file path\n  --lang           Select language (it or en)\n")
        return

    if args.create_daemon:
        create_systemd_service()
        return

    if is_daemon_running():
        message = "SySeBa is running!"
        if args.silent:
            logging.error(message) 
        else:
            print(message) 
        exit(1) 

    source, backup, restore, log_file, threads = load_config(args.config)
    lang = load_language(lang="it" if args.lang == "it" else "en")

    initialize_database()

    os.makedirs(os.path.dirname(log_file), exist_ok=True)
    log_queue = Queue()
    log_thread = threading.Thread(target=log_writer, args=(log_queue, log_file))
    log_thread.daemon = True
    log_thread.start()

    queue = Queue()
    for _ in range(threads):
        t = threading.Thread(target=worker_thread, args=(queue, source, backup, restore, log_queue))
        t.daemon = True
        t.start()

    event_handler = BackupHandler(source, backup, restore, queue)
    observer = Observer()
    observer.schedule(event_handler, source, recursive=True)

    initial_sync_thread = threading.Thread(target=initial_sync, args=(source, backup, log_queue))
    initial_sync_thread.start()

    stop_event = threading.Event()
    try:
        observer.start()
        display_status(source, backup, log_file, stop_event, lang)
    except KeyboardInterrupt:
        stop_event.set()
    finally:
        observer.stop()
        observer.join()
        log_queue.put("STOP")
        log_thread.join()
        initial_sync_thread.join()

if __name__ == "__main__":
    main()
