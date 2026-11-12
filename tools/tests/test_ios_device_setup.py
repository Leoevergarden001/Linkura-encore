import datetime as dt
import tempfile
import unittest
from pathlib import Path

from tools import ios_device_setup as setup


def write(root: Path, rel: str, data: bytes = b"x") -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)


class BuildGroupsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        write(self.root, "Documents/D/00/aa", b"12345")
        write(self.root, "Documents/D/00/sub/bb", b"12")
        write(self.root, "Documents/D/00/._aa", b"\x00\x05\x16\x07")
        write(self.root, "Documents/D/01/cc", b"1")
        write(self.root, "Documents/D/._01", b"\x00\x05\x16\x07")
        write(self.root, "Documents/M/master", b"123")
        write(self.root, "Documents/M/.DS_Store", b"j")
        write(self.root, "Documents/LinkuraOffline/assets/raw/pw/x", b"1")
        write(self.root, "Documents/LinkuraOffline/storage-key.json", b"{}")
        write(self.root, "Documents/LinkuraOffline/config.json", b"{}")
        write(self.root, "Documents/LinkuraOffline/server.log", b"debug")
        write(self.root, "Library/Application Support/LinkuraOffline/HasuKikaisann.sqlite3", b"db")

    def tearDown(self):
        self.tmp.cleanup()

    def test_groups_skip_junk_and_debug_files(self):
        groups = setup.build_groups(self.root, include_progress=False)
        files = {f for g in groups for f in g.files}
        self.assertEqual(files, {
            "Documents/D/00/aa", "Documents/D/00/sub/bb", "Documents/D/01/cc",
            "Documents/M/master", "Documents/LinkuraOffline/assets/raw/pw/x",
            "Documents/LinkuraOffline/storage-key.json", "Documents/LinkuraOffline/config.json",
        })
        d00 = next(g for g in groups if g.dest == "Documents/D/00")
        self.assertTrue(d00.is_dir and d00.has_junk)
        self.assertEqual(d00.size, 7)

    def test_progress_only_when_requested(self):
        groups = setup.build_groups(self.root, include_progress=True)
        self.assertIn(setup.PROGRESS_DB, {g.dest for g in groups})

    def test_missing_storage_key_is_an_error(self):
        (self.root / "Documents/LinkuraOffline/storage-key.json").unlink()
        with self.assertRaises(setup.SetupError):
            setup.build_groups(self.root, include_progress=False)

    def test_compare_marks_missing_and_mismatched(self):
        groups = setup.build_groups(self.root, include_progress=False)
        setup.compare(groups, {"Documents/D/00/aa": 5, "Documents/D/00/sub/bb": 1, "Documents/M/master": 9})
        d00 = next(g for g in groups if g.dest == "Documents/D/00")
        # A cache file with a different size is a partial push: re-push it.
        self.assertEqual(d00.missing, ["Documents/D/00/sub/bb"])
        self.assertEqual(d00.mismatched, [])
        master = next(g for g in groups if g.dest == "Documents/M/master")
        self.assertEqual((master.missing, master.mismatched), ([], ["Documents/M/master"]))
        d01 = next(g for g in groups if g.dest == "Documents/D/01")
        self.assertEqual(d01.missing, ["Documents/D/01/cc"])


class PushTest(unittest.TestCase):
    def test_vanished_backup_stops_without_retrying(self):
        calls = []
        orig = setup.devicectl
        setup.devicectl = lambda *a, **k: calls.append(a)
        try:
            dev = setup.Device("U", "n", "m", "26", True, "enabled", None)
            with self.assertRaises(setup.SetupError) as cm:
                setup.push(dev, "b", Path("/Volumes/__gone__/x"), "Documents/D/x")
        finally:
            setup.devicectl = orig
        self.assertEqual(calls, [])
        self.assertIn("备份盘", str(cm.exception))


    def test_device_full_stops_without_retrying(self):
        calls = []

        def fail(*a, **k):
            calls.append(a)
            raise setup.SetupError("openat(2) returned POSIX error code 28 (NSPOSIXErrorDomain error 28 (0x1C))")
        orig = setup.devicectl
        setup.devicectl = fail
        try:
            dev = setup.Device("U", "n", "m", "26", True, "enabled", None)
            with self.assertRaises(setup.DeviceFull):
                setup.push(dev, "b", Path(__file__), "Documents/D/x")
        finally:
            setup.devicectl = orig
        self.assertEqual(len(calls), 1)


class ProfileTest(unittest.TestCase):
    future = dt.datetime.now() + dt.timedelta(days=3)

    def profile(self, app_id="TEAM.org.example", devices=("UDID1",), expires=None):
        return {"Entitlements": {"application-identifier": app_id},
                "ProvisionedDevices": list(devices), "ExpirationDate": expires or self.future}

    def test_matches_bundle_and_device(self):
        self.assertTrue(setup.profile_covers(self.profile(), "org.example", "UDID1"))
        self.assertTrue(setup.profile_covers(self.profile(app_id="TEAM.*"), "org.example", "UDID1"))

    def test_rejects_other_device_bundle_or_expired(self):
        self.assertFalse(setup.profile_covers(self.profile(), "org.example", "UDID2"))
        self.assertFalse(setup.profile_covers(self.profile(), "org.other", "UDID1"))
        expired = self.profile(expires=dt.datetime.now() - dt.timedelta(days=30))
        self.assertFalse(setup.profile_covers(expired, "org.example", "UDID1"))


class LaunchTest(unittest.TestCase):
    def _launch(self, error):
        def fake(*a, **k):
            raise setup.SetupError(error)
        orig = setup.devicectl
        setup.devicectl = fake
        try:
            dev = setup.Device("U", "iPhone", "m", "27", True, "enabled", None)
            with self.assertRaises(setup.SetupError) as cm:
                setup.launch_app(dev, "b")
        finally:
            setup.devicectl = orig
        return str(cm.exception)

    def test_untrusted_developer_explains_the_settings_step(self):
        msg = self._launch("devicectl device process launch 失败: Unable to launch b because it has an "
                           "invalid code signature, inadequate entitlements or its profile has not been "
                           "explicitly trusted by the user. (FBSOpenApplicationErrorDomain error 3 (0x03))")
        self.assertIn("VPN 与设备管理", msg)
        self.assertIn("--skip-install", msg)

    def test_other_launch_errors_pass_through(self):
        self.assertEqual(self._launch("devicectl device process launch 失败: busy"),
                         "devicectl device process launch 失败: busy")


if __name__ == "__main__":
    unittest.main()
