import json
import os
import re
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
from unittest import mock

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

    def test_process_lock_accepts_a_relative_filename(self):
        with tempfile.TemporaryDirectory() as tmp:
            original_directory = os.getcwd()
            try:
                os.chdir(tmp)
                lock = syseba.ProcessLock("syseba.lock")
                acquired, pid = lock.acquire()
                self.assertTrue(acquired)
                self.assertEqual(pid, os.getpid())
                lock.release()
                self.assertFalse(Path("syseba.lock").exists())
            finally:
                os.chdir(original_directory)

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
            download_payload = (b"SySeBa-stream-test\n" * 65537) + b"done"
            (root / "restore" / "download.bin").write_bytes(download_payload)
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

                with urllib.request.urlopen(f"http://127.0.0.1:{port}/webui.js", timeout=2) as response:
                    web_script = response.read().decode("utf-8")
                    content_security_policy = response.headers.get("Content-Security-Policy", "")
                self.assertIn("restore-overwrite", web_script)
                self.assertIn("script-src 'self'", content_security_policy)

                download_request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/restore/download?path=download.bin",
                    headers={"X-SySeBa-Token": "test-token"},
                )
                with urllib.request.urlopen(download_request, timeout=5) as response:
                    downloaded = response.read()
                    content_length = int(response.headers["Content-Length"])
                    disposition = response.headers.get("Content-Disposition", "")
                self.assertEqual(downloaded, download_payload)
                self.assertEqual(content_length, len(download_payload))
                self.assertIn('filename="download.bin"', disposition)

                state_request = urllib.request.Request(
                    f"http://127.0.0.1:{port}/api/config/state",
                    headers={"X-SySeBa-Token": "test-token"},
                )
                with urllib.request.urlopen(state_request, timeout=2) as response:
                    config_state = json.loads(response.read().decode("utf-8"))
                self.assertFalse(config_state["restart_required"])
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()


class SySeBaUxTests(unittest.TestCase):
    def write_config(self, root, threads=1):
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
            f"threads = {threads}\n",
            encoding="utf-8",
        )
        return config

    def test_initial_sync_states_distinguish_skipped_web_only_and_completed(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = syseba.SySeBaConfig.load(self.write_config(root))

            skipped = syseba.SySeBaDaemon(config, no_initial_sync=True)
            skipped_status = skipped.status()["initial_sync"]
            self.assertEqual(skipped_status["state"], "skipped")
            self.assertIsNone(skipped_status["percent"])

            web_only = syseba.SySeBaDaemon(config, db_path=str(root / "web.db"))
            web_only.start_web_only()
            try:
                web_status = web_only.status()["initial_sync"]
                self.assertEqual(web_status["state"], "not_available")
                self.assertIsNone(web_status["percent"])
            finally:
                web_only.stop()

            completed = syseba.SySeBaDaemon(config)
            completed.initial_sync()
            completed_status = completed.status()["initial_sync"]
            self.assertEqual(completed_status["state"], "completed")
            self.assertEqual(completed_status["percent"], 100.0)

    def test_restore_supports_pagination_search_rename_and_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = syseba.SySeBaConfig.load(self.write_config(root))
            restore_root = Path(config.restore)
            source_root = Path(config.source)
            for index in range(6):
                (restore_root / f"file-{index}.txt").write_text(f"restore-{index}", encoding="utf-8")
            (source_root / "file-0.txt").write_text("source-original", encoding="utf-8")

            daemon = syseba.SySeBaDaemon(config)
            with self.assertRaises(ValueError):
                daemon.restore_item("")
            with self.assertRaises(ValueError):
                daemon.restore_item_info("")

            first_page = daemon.list_restore(page=1, page_size=2)
            self.assertEqual(first_page["total"], 6)
            self.assertEqual(first_page["pages"], 3)
            self.assertTrue(first_page["has_next"])
            self.assertEqual(len(first_page["items"]), 2)

            filtered = daemon.list_restore(search="file-5")
            self.assertEqual([item["name"] for item in filtered["items"]], ["file-5.txt"])
            self.assertTrue(daemon.restore_item_info("file-0.txt")["destination_exists"])

            with self.assertRaises(FileExistsError):
                daemon.restore_item("file-0.txt")

            renamed = Path(daemon.restore_item("file-0.txt", strategy="rename"))
            self.assertNotEqual(renamed, source_root / "file-0.txt")
            self.assertEqual(renamed.read_text(encoding="utf-8"), "restore-0")
            self.assertEqual((source_root / "file-0.txt").read_text(encoding="utf-8"), "source-original")

            overwritten = Path(daemon.restore_item("file-0.txt", strategy="overwrite"))
            self.assertEqual(overwritten, source_root / "file-0.txt")
            self.assertEqual(overwritten.read_text(encoding="utf-8"), "restore-0")

    def test_modify_event_deleted_during_debounce_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = syseba.SySeBaConfig.load(self.write_config(root))
            source_file = Path(config.source) / "race.txt"
            source_file.write_text("temporary", encoding="utf-8")
            daemon = syseba.SySeBaDaemon(config)

            def delete_during_debounce(_seconds):
                source_file.unlink()

            with mock.patch.object(syseba.time, "sleep", side_effect=delete_during_debounce):
                daemon.process_event("modify", str(source_file))

            self.assertEqual(daemon.stats["errors"], 0)
            self.assertEqual(daemon.stats["skipped"], 1)

    def test_config_state_separates_active_and_saved_values(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = syseba.SySeBaConfig.load(self.write_config(root))
            daemon = syseba.SySeBaDaemon(config)
            state = daemon.config_state()
            self.assertFalse(state["restart_required"])

            config.save({"threads": 7})
            changed = daemon.config_state()
            self.assertTrue(changed["restart_required"])
            self.assertEqual(changed["active"]["threads"], 1)
            self.assertEqual(changed["saved"]["threads"], 7)
            self.assertIn("threads", changed["changes"])

    def test_console_layout_respects_terminal_width_and_height(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = syseba.SySeBaConfig.load(self.write_config(root))
            daemon = syseba.SySeBaDaemon(config, lang=syseba.load_language(lang="it"), no_initial_sync=True)
            status = daemon.status()
            status["recent_logs"] = [
                "2026-07-23 00:00:00 [INFO] /a/very/long/path/that/must/not/overflow/the/terminal.txt"
            ] * 12
            dashboard = syseba.ConsoleDashboard(daemon)
            for width, height in ((40, 12), (60, 20), (80, 24), (100, 30)):
                with self.subTest(width=width, height=height):
                    lines = dashboard.build_lines(status, width, height)
                    self.assertLessEqual(len(lines), height)
                    self.assertTrue(lines)
                    self.assertLessEqual(max(len(re.sub(r"\x1b\[[0-9;]*m", "", line)) for line in lines), width)
            self.assertIn("saltata", "\n".join(dashboard.build_lines(status, 80, 24)))

    def test_dashboard_markup_is_accessible_and_uses_external_script(self):
        html = syseba.render_dashboard_html("en")
        self.assertIn('<html lang="en">', html)
        self.assertIn('role="tablist"', html)
        self.assertIn('aria-live="polite"', html)
        self.assertIn('src="/webui.js"', html)
        self.assertNotIn("onclick=", html)

    def test_cli_config_check_and_restore_list_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config_path = self.write_config(root)
            (root / "restore" / "cli-item.txt").write_text("cli", encoding="utf-8")
            script = str(Path(__file__).resolve().parents[1] / "syseba.py")

            check = subprocess.run(
                [sys.executable, script, "config-check", "--config", str(config_path), "--json"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(check.returncode, 0, check.stderr)
            self.assertTrue(json.loads(check.stdout)["ok"])

            listing = subprocess.run(
                [sys.executable, script, "restore-list", "--config", str(config_path), "--json"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(listing.returncode, 0, listing.stderr)
            self.assertEqual(json.loads(listing.stdout)["items"][0]["name"], "cli-item.txt")

            file_listing = subprocess.run(
                [sys.executable, script, "restore-list", "--config", str(config_path), "--path", "cli-item.txt"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(file_listing.returncode, 0, file_listing.stderr)
            self.assertIn("FILE", file_listing.stdout)
            self.assertIn("cli-item.txt", file_listing.stdout)


if __name__ == "__main__":
    unittest.main()
