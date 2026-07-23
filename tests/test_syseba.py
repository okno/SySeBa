import json
import os
import re
import shutil
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

    def test_explicit_missing_config_never_falls_back_to_a_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing = Path(tmp) / "missing.conf"
            with self.assertRaisesRegex(FileNotFoundError, "specificato non trovato"):
                syseba.SySeBaConfig.load(str(missing), language="it")
            with self.assertRaisesRegex(FileNotFoundError, "Specified config file not found"):
                syseba.SySeBaConfig.load(str(missing), language="en")

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
                auth_url = f"http://127.0.0.1:{port}/api/auth"
                for _ in range(30):
                    try:
                        with urllib.request.urlopen(auth_url, timeout=0.2) as response:
                            auth_state = json.loads(response.read().decode("utf-8"))
                        break
                    except OSError:
                        time.sleep(0.1)
                else:
                    self.fail("web server did not expose its authentication state")
                self.assertTrue(auth_state["required"])

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

    def test_web_token_file_is_persistent_and_private(self):
        with tempfile.TemporaryDirectory() as tmp:
            token_file = Path(tmp) / "syseba_web.token"
            first_token, first_path, first_created = syseba.ensure_web_token_file(str(token_file))
            second_token, second_path, second_created = syseba.ensure_web_token_file(str(token_file))

            self.assertTrue(first_created)
            self.assertFalse(second_created)
            self.assertEqual(first_token, second_token)
            self.assertEqual(first_path, second_path)
            self.assertEqual(token_file.read_text(encoding="utf-8").strip(), first_token)
            self.assertGreaterEqual(len(first_token), 32)
            if os.name != "nt":
                self.assertEqual(token_file.stat().st_mode & 0o777, 0o600)

    def test_web_token_file_rejects_symbolic_links_when_supported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            target = root / "target.token"
            target.write_text("do-not-read\n", encoding="utf-8")
            link = root / "syseba_web.token"
            try:
                os.symlink(target, link)
            except (OSError, NotImplementedError):
                self.skipTest("symlinks are not available for this test user")

            with self.assertRaises(ValueError):
                syseba.ensure_web_token_file(str(link))

    def test_systemd_service_always_enables_protected_lan_web(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            install_dir = root / "syseba"
            install_dir.mkdir()
            unit_path = root / "syseba.service"
            token_path = install_dir / "syseba_web.token"

            with mock.patch.object(syseba.subprocess, "run") as run, mock.patch("builtins.print"):
                result = syseba.create_systemd_service(
                    web_host="0.0.0.0",
                    web_port=8765,
                    web_token_file=str(token_path),
                    language="en",
                    install_dir=str(install_dir),
                    unit_path=str(unit_path),
                    python_executable="/usr/bin/python3",
                )

            unit = unit_path.read_text(encoding="utf-8")
            self.assertIn("Wants=network-online.target", unit)
            self.assertIn("Environment=PYTHONUNBUFFERED=1", unit)
            self.assertIn("--silent --web --web-host 0.0.0.0 --web-port 8765 --lang en", unit)
            self.assertIn(f"--web-token-file {syseba.shlex.quote(str(token_path))}", unit)
            self.assertNotIn("--no-web-auth", unit)
            self.assertTrue(token_path.is_file())
            self.assertTrue(result["token_created"])
            self.assertEqual(result["web_port"], 8765)
            run.assert_has_calls([
                mock.call(["systemctl", "daemon-reload"], check=True),
                mock.call(["systemctl", "enable", "syseba.service"], check=True),
            ])

    def test_quick_update_restores_old_unit_when_web_migration_fails(self):
        if os.name == "nt":
            candidates = [
                Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "bin" / "bash.exe",
                Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git" / "usr" / "bin" / "bash.exe",
            ]
            bash = next((str(path) for path in candidates if path.exists()), None)
        else:
            bash = shutil.which("bash")
        if not bash:
            self.skipTest("bash is not available")

        script = r'''
set -Eeuo pipefail
source ./syseba-maintenance.sh

fixture="$(mktemp -d)"
INSTALL_DIR="$fixture/install"
UNIT_FILE="$fixture/syseba.service"
SERVICE_NAME="syseba.service"
SYSTEMCTL_BIN="true"
mkdir -p "$INSTALL_DIR"
printf 'placeholder\n' > "$INSTALL_DIR/syseba.py"
printf 'old-unit\n' > "$UNIT_FILE"

resolve_target_commit() { printf 'same-commit\n'; }
installed_identity() { printf 'same-commit\n'; }
verify_install() { return 0; }
service_is_active() { return 0; }
stop_service() { return 0; }
ensure_no_unmanaged_process() { return 0; }
configure_web_service() { printf 'new-web-unit\n' > "$UNIT_FILE"; }
cmd_logs() { return 0; }

start_count=0
start_service() {
    start_count=$((start_count + 1))
    if [[ "$start_count" -eq 1 ]]; then
        return 1
    fi
    grep -Fqx 'old-unit' "$UNIT_FILE"
}

set +e
(
    cmd_quick_update main
)
status=$?
set -e

grep -Fqx 'old-unit' "$UNIT_FILE"
rm -rf -- "$fixture"
[[ "$status" -eq 1 ]]
'''
        result = subprocess.run(
            [bash, "-c", script],
            cwd=Path(__file__).resolve().parents[1],
            capture_output=True,
            text=True,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


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

    def test_invalid_config_update_is_rejected_without_changing_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config_path = self.write_config(root)
            config = syseba.SySeBaConfig.load(config_path)
            original = config_path.read_text(encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "cannot contain one another"):
                config.save({"backup": str(Path(config.source) / "nested")}, language="en")

            self.assertEqual(config_path.read_text(encoding="utf-8"), original)

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
        self.assertIn(".hide { display: none !important; }", html)
        self.assertNotIn("onclick=", html)

    def test_language_file_covers_console_and_cli_labels(self):
        italian = syseba.load_language(lang="it")
        english = syseba.load_language(lang="en")
        expected = {
            "CLOCK", "STATUS", "RUNNING", "STOPPED", "SOURCE", "BACKUP", "RESTORE",
            "INITIAL_SYNC", "WARNING", "ERROR", "FILE", "DIRECTORY", "ITEMS", "PAGE",
        }
        self.assertTrue(expected.issubset(italian))
        self.assertTrue(expected.issubset(english))
        self.assertEqual(italian["STOPPED"], "FERMO")
        self.assertEqual(english["STOPPED"], "STOPPED")

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
