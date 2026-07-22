import json
import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import syseba


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class SySeBaSecurityTests(unittest.TestCase):
    def write_config(self, root):
        source = root / "source"
        backup = root / "backup"
        restore = root / "restore"
        for path in (source, backup, restore):
            path.mkdir(parents=True, exist_ok=True)
        config = root / "syseba.conf"
        config.write_text(
            "[SETTINGS]\n"
            f"source = {source}\n"
            f"backup = {backup}\n"
            f"restore = {restore}\n"
            f"log = {root / 'syseba.log'}\n"
            "threads = 1\n",
            encoding="utf-8",
        )
        return config

    def test_safe_join_blocks_traversal_but_allows_legitimate_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp) / "restore"
            base.mkdir()
            allowed = syseba.safe_join(str(base), "..backup/file.txt")
            self.assertTrue(syseba.is_inside_base(str(base), allowed))
            self.assertTrue(allowed.endswith(os.path.join("..backup", "file.txt")))

            with self.assertRaises(ValueError):
                syseba.safe_join(str(base), "../escape.txt")

    def test_safe_join_blocks_symlink_escape_when_supported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            base = root / "restore"
            outside = root / "outside"
            base.mkdir()
            outside.mkdir()
            link = base / "link"
            try:
                os.symlink(outside, link, target_is_directory=True)
            except (OSError, NotImplementedError):
                self.skipTest("symlinks are not available for this test user")

            with self.assertRaises(ValueError):
                syseba.safe_join(str(base), "link/file.txt")

    def test_old_sqlite_schema_is_migrated(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = syseba.SySeBaConfig.load(self.write_config(root))
            db_path = root / "old.db"
            connection = sqlite3.connect(db_path)
            try:
                connection.execute(
                    """
                    CREATE TABLE logs (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        timestamp TEXT,
                        operation TEXT,
                        source_path TEXT,
                        target_path TEXT,
                        additional_info TEXT
                    )
                    """
                )
                connection.commit()
            finally:
                connection.close()

            daemon = syseba.SySeBaDaemon(config, db_path=str(db_path))
            daemon.initialize_database()
            connection = sqlite3.connect(db_path)
            try:
                columns = [row[1] for row in connection.execute("PRAGMA table_info(logs)").fetchall()]
                self.assertIn("level", columns)
            finally:
                connection.close()

    def test_tail_file_reads_last_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "syseba.log"
            log_path.write_text("\n".join(f"line-{index}" for index in range(50)), encoding="utf-8")
            self.assertEqual(syseba.tail_file(str(log_path), 3), ["line-47", "line-48", "line-49"])

    def test_web_only_persists_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = syseba.SySeBaConfig.load(self.write_config(root))
            db_path = root / "syseba.db"
            daemon = syseba.SySeBaDaemon(config, db_path=str(db_path))
            daemon.start_web_only()
            try:
                daemon.emit("INFO", "web-only persisted", "TEST")
                daemon.log_queue.join()
            finally:
                daemon.stop()

            self.assertIn("web-only persisted", Path(config.log_file).read_text(encoding="utf-8"))
            connection = sqlite3.connect(db_path)
            try:
                rows = connection.execute("SELECT operation FROM logs WHERE operation = 'TEST'").fetchall()
            finally:
                connection.close()
            self.assertEqual(rows, [("TEST",)])

    def test_web_api_requires_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = self.write_config(root)
            db_path = root / "syseba.db"
            port = free_port()
            process = subprocess.Popen(
                [
                    sys.executable,
                    str(Path(__file__).resolve().parents[1] / "syseba.py"),
                    "--web-only",
                    "--web-host",
                    "127.0.0.1",
                    "--web-port",
                    str(port),
                    "--web-token",
                    "test-token",
                    "--config",
                    str(config),
                    "--db-path",
                    str(db_path),
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                url = f"http://127.0.0.1:{port}/api/status"
                for _ in range(30):
                    try:
                        urllib.request.urlopen(url, timeout=0.2)
                    except urllib.error.HTTPError as error:
                        if error.code == 401:
                            error.close()
                            break
                        error.close()
                    except OSError:
                        time.sleep(0.1)
                else:
                    self.fail("web server did not start with protected API")

                with self.assertRaises(urllib.error.HTTPError) as context:
                    urllib.request.urlopen(url, timeout=2)
                self.assertEqual(context.exception.code, 401)
                context.exception.close()

                request = urllib.request.Request(url, headers={"X-SySeBa-Token": "test-token"})
                with urllib.request.urlopen(request, timeout=2) as response:
                    payload = json.loads(response.read().decode("utf-8"))
                self.assertTrue(payload["web_only"])
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()


if __name__ == "__main__":
    unittest.main()
