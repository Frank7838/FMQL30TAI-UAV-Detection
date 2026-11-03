"""用临时会话目录检查记录清理逻辑。"""
from pathlib import Path
import json
import os
import subprocess
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src/board/runtime"))
import session_retention as retention

CURRENT = str(uuid.UUID(int=10))
OLD = str(uuid.UUID(int=9))


class SessionRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "sessions").mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def session(self, name, boot=OLD, closed=False):
        path = self.root / "sessions" / name
        path.mkdir()
        (path / "state.json").write_text(json.dumps(dict(boot=boot, phase="RUNNING", frames=37)))
        if closed:
            (path / "result.json").write_text(json.dumps(dict(boot=boot, device_closed=True)))
        return path

    def test_nine_abrupt_old_boots_do_not_block_start(self):
        for n in range(9):
            self.session("camera_old_%02d" % n)
        report = retention.collect_history(self.root, CURRENT)
        self.assertEqual(report["pruned_count"], 5)
        self.assertEqual(len(list((self.root / "sessions").iterdir())), 4)
        self.assertTrue(all(x["kind"] == "INTERRUPTED_PREVIOUS_BOOT" for x in report["pruned"]))

    def test_twenty_five_cold_starts_stay_bounded(self):
        for n in range(25):
            current = str(uuid.UUID(int=100 + n))
            retention.collect_history(self.root, current)
            path = self.root / "sessions" / ("camera_demo_b" + current.replace("-", ""))
            retention.prepare_session(path, current)
            # Same exclusive creation as the existing, unmodified board worker.
            path.mkdir(parents=True, exist_ok=False)
            self.assertLessEqual(len(list((self.root / "sessions").iterdir())), 5)

    def test_current_boot_without_sdk_close_is_retained(self):
        path = self.session("camera_current", CURRENT)
        with self.assertRaisesRegex(RuntimeError, "Unresolved session ownership"):
            retention.collect_history(self.root, CURRENT)
        self.assertTrue(path.is_dir())

    def test_closed_same_boot_sessions_can_rotate(self):
        for n in range(10):
            self.session("camera_closed_%02d" % n, CURRENT, True)
        self.assertEqual(retention.collect_history(self.root, CURRENT)["pruned_count"], 6)

    def test_non_session_files_are_not_a_start_limit(self):
        for n in range(12):
            (self.root / "sessions" / ("note_%02d.txt" % n)).write_text("keep")
        self.assertEqual(retention.collect_history(self.root, CURRENT)["ignored_entries"], 12)
        self.assertEqual(len(list((self.root / "sessions").iterdir())), 12)

    def test_calendar_rollback_does_not_discard_last_run(self):
        for n in range(9):
            self.session("camera_legacy_%02d" % n)
        path = self.session("camera_last", CURRENT, True)
        os.utime(path, (1, 1))
        (self.root / "ACTIVE_RUN.json").write_text(json.dumps(dict(session=path.name, boot=CURRENT)))
        report = retention.collect_history(self.root, CURRENT)
        self.assertEqual(report["retained"][0]["name"], path.name)
        self.assertTrue(path.exists())

    def test_unknown_marker_blocks_without_change(self):
        for n in range(9):
            self.session("camera_old_%02d" % n)
        (self.root / "UNKNOWN.json").write_text("{}")
        with self.assertRaisesRegex(RuntimeError, "UNKNOWN.json"):
            retention.collect_history(self.root, CURRENT)
        self.assertEqual(len(list((self.root / "sessions").iterdir())), 9)

    def test_conflicting_or_corrupt_metadata_is_not_deleted(self):
        path = self.session("camera_conflict", OLD, True)
        (path / "owner.json").write_text(json.dumps(dict(boot=CURRENT)))
        with self.assertRaisesRegex(RuntimeError, "Unresolved session ownership"):
            retention.collect_history(self.root, CURRENT)
        (path / "owner.json").write_text("bad-json")
        with self.assertRaises(RuntimeError):
            retention.collect_history(self.root, CURRENT)
        self.assertTrue(path.exists())

    def test_boot_suffix_recovers_cut_before_state_write(self):
        path = self.root / "sessions" / ("camera_empty_b" + OLD.replace("-", ""))
        path.mkdir()
        self.assertEqual(retention.classify(path, CURRENT)["kind"], "INTERRUPTED_PREVIOUS_BOOT")
        self.assertEqual(retention.classify(path, OLD)["kind"], "UNRESOLVED_CURRENT_BOOT")

    def test_unclassified_legacy_kept_with_named_error(self):
        path = self.root / "sessions" / "camera_legacy"
        path.mkdir()
        with self.assertRaisesRegex(RuntimeError, "camera_legacy"):
            retention.collect_history(self.root, CURRENT)
        self.assertTrue(path.is_dir())

    @unittest.skipUnless(os.name == "posix", "Linux flock regression")
    def test_live_owner_lock_prevents_collection(self):
        lock = self.root / "test.lock"
        lock.write_text("")
        code = "import fcntl,sys; f=open(sys.argv[1],'r+'); fcntl.flock(f,fcntl.LOCK_EX); print('LOCKED',flush=True); sys.stdin.read()"
        process = subprocess.Popen([sys.executable, "-B", "-c", code, str(lock)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(process.stdout.readline().strip(), "LOCKED")
            with self.assertRaisesRegex(RuntimeError, "Device owner still holds"):
                with retention.device_guard(lock, lock.stat().st_ino):
                    self.fail("Must not enter collection")
        finally:
            process.communicate("", timeout=5)
        with retention.device_guard(lock, lock.stat().st_ino):
            self.assertTrue(lock.exists())

    @unittest.skipUnless(os.name == "posix", "Linux symlink regression")
    def test_outside_symlink_is_untouched(self):
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "important.txt").write_text("keep")
        (self.root / "sessions" / "camera_link").symlink_to(outside, target_is_directory=True)
        retention.collect_history(self.root, CURRENT)
        self.assertEqual((outside / "important.txt").read_text(), "keep")


if __name__ == "__main__":
    unittest.main(verbosity=2)
