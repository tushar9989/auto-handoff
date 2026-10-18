"""Tests for install.py against a throwaway Claude config folder. Run: make test"""
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
INSTALL = os.path.join(HERE, "..", "install.py")


class InstallTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.config = os.path.join(self.tmp.name, ".claude")
        os.makedirs(self.config)
        self.settings = os.path.join(self.config, "settings.json")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, settings):
        with open(self.settings, "w") as f:
            json.dump(settings, f)

    def read(self):
        with open(self.settings) as f:
            return json.load(f)

    def run_install(self, *args, ok=True):
        env = dict(os.environ, CLAUDE_CONFIG_DIR=self.config)
        out = subprocess.run([sys.executable, INSTALL] + list(args), capture_output=True, text=True, env=env)
        self.assertEqual(out.returncode == 0, ok, out.stderr)
        return out

    def test_round_trip_restores_exactly(self):
        original = {"model": "opus", "hooks": {}, "permissions": {"allow": []},
                    "modelSettings": {"claude-opus-5-5": {"effortLevel": "xhigh", "autoCompactWindow": 1000000}}}
        self.write(original)
        self.run_install()
        installed = self.read()
        self.assertEqual(installed["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"], 333000)
        self.assertEqual(installed["modelSettings"]["claude-fable-5-1"]["autoCompactWindow"], 333000)
        self.assertEqual(installed["modelSettings"]["claude-opus-4-8"]["autoCompactWindow"], 333000)
        self.assertIs(installed["idleCompaction"], False)
        self.assertIs(installed["precomputeCompactionEnabled"], False)
        self.assertEqual(len(installed["hooks"]["PreCompact"]), 1)
        self.run_install()  # idempotent
        self.assertEqual(self.read(), installed)
        self.run_install("--uninstall")
        self.assertEqual(self.read(), original)

    def test_a_hook_sharing_our_entry_is_kept(self):
        mine = {"type": "command", "command": "say done"}
        self.write({"hooks": {"Stop": [{"hooks": [mine]}]}})
        self.run_install()
        installed = self.read()
        installed["hooks"]["Stop"][-1]["hooks"].append(mine)  # merged by hand into our entry
        self.write(installed)
        self.run_install("--uninstall")
        commands = [h["command"] for e in self.read()["hooks"]["Stop"] for h in e["hooks"]]
        self.assertEqual(commands, ["say done", "say done"])

    def test_round_trip_from_bare_settings(self):
        self.write({"model": "opus"})
        self.run_install()
        self.run_install("--uninstall")
        self.assertEqual(self.read(), {"model": "opus"})

    def test_uninstall_keeps_what_it_did_not_add(self):
        mine = {"hooks": [{"type": "command", "command": "~/bin/my-auto-handoff-notes"}]}
        rule = "Read(~/.local/state/claude-handoffs/**)"
        self.write({"hooks": {"Stop": [mine]}, "permissions": {"allow": [rule]}, "idleCompaction": True})
        self.run_install()
        self.run_install("--uninstall")
        after = self.read()
        self.assertEqual(after["hooks"]["Stop"], [mine])
        self.assertEqual(after["permissions"]["allow"], [rule])
        self.assertIs(after["idleCompaction"], True)

    def test_uninstall_leaves_later_window_changes(self):
        self.write({"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": 1000000}}})
        self.run_install()
        changed = self.read()
        changed["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"] = 500000  # /autocompact 500k
        self.write(changed)
        self.run_install("--uninstall")
        self.assertEqual(self.read()["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"], 500000)

    def test_reinstall_keeps_later_changes(self):
        self.write({"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": 1000000}}})
        self.run_install()
        changed = self.read()
        changed["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"] = 500000
        changed["idleCompaction"] = True
        self.write(changed)
        self.run_install()  # say, after a rebuild
        after = self.read()
        self.assertEqual(after["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"], 500000)
        self.assertIs(after["idleCompaction"], True)
        self.run_install()
        self.run_install("--uninstall")
        after = self.read()
        self.assertEqual(after["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"], 500000)
        self.assertIs(after["idleCompaction"], True)

    def test_reinstall_after_restoring_the_backup_manages_again(self):
        original = {"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": 1000000}}, "idleCompaction": True}
        self.write(original)
        self.run_install()
        self.write(original)  # the backup restored by hand
        out = self.run_install()
        after = self.read()
        self.assertEqual(after["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"], 333000)
        self.assertIs(after["idleCompaction"], False)
        self.assertNotIn("Left as you set them", out.stdout)
        self.run_install("--uninstall")
        self.assertEqual(self.read(), original)

    def test_reinstall_says_what_it_left(self):
        self.write({"modelSettings": {"claude-opus-5-5": {"autoCompactWindow": 1000000}}})
        self.run_install()
        changed = self.read()
        changed["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"] = 500000
        self.write(changed)
        self.assertIn("Left as you set them since installing: claude-opus-5-5", self.run_install().stdout)
        changed["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"] = 1000000  # back to the original
        self.write(changed)
        self.run_install()
        self.assertEqual(self.read()["modelSettings"]["claude-opus-5-5"]["autoCompactWindow"], 333000)

    def test_reinstall_swaps_in_a_new_binary_file(self):
        self.write({"model": "opus"})
        self.run_install()
        binary = os.path.join(self.config, "hooks", "auto-handoff")
        first = os.stat(binary).st_ino
        self.run_install()
        self.assertNotEqual(os.stat(binary).st_ino, first)  # never rewritten in place
        self.assertFalse(os.path.exists(binary + ".new"))

    def test_no_settings_file(self):
        self.run_install()
        self.assertIn("PreCompact", self.read()["hooks"])
        self.run_install("--uninstall")
        self.assertEqual(self.read(), {})

    def test_bad_settings_are_left_alone(self):
        for text in ('{\n  "model": "opus",\n}\n', '{"hooks": {"Stop": ["oops"]}}', '{"permissions": {"allow": "Read"}}',
                     '{"modelSettings": {"claude-opus-5-5": "auto"}}', "[]"):
            with open(self.settings, "w") as f:
                f.write(text)
            out = self.run_install(ok=False)
            self.assertIn("Nothing was changed", out.stderr)
            with open(self.settings) as f:
                self.assertEqual(f.read(), text)
            self.assertFalse(os.path.exists(os.path.join(self.config, "hooks", "auto-handoff")))

    def test_symlink_and_permissions_are_kept(self):
        real = os.path.join(self.tmp.name, "dotfiles-settings.json")
        with open(real, "w") as f:
            json.dump({"model": "opus"}, f)
        os.chmod(real, 0o600)
        os.symlink(real, self.settings)
        self.run_install()
        self.assertTrue(os.path.islink(self.settings))
        self.assertEqual(os.stat(real).st_mode & 0o777, 0o600)
        with open(real) as f:
            self.assertIn("hooks", json.load(f))


if __name__ == "__main__":
    unittest.main()
