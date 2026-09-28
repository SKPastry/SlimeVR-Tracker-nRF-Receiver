#!/usr/bin/env python3
"""Exercise heated-start parser boundaries without USB or radio hardware."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import types
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.modules.setdefault("hid", types.ModuleType("hid"))
spec = importlib.util.spec_from_file_location("hid_cmd", ROOT / "scripts/hid_cmd.py")
sender = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sender)


class HeatedSend(unittest.TestCase):
    def test_start_target_and_alias(self):
        for target, tid in (("0", 0), ("7", 7), ("all", 255)):
            for tokens in (["tcal", "heat", "start"], ["tcal-heat-start"]):
                with self.subTest(target=target, tokens=tokens):
                    self.assertEqual(sender.build_send(target, tokens), (0x27, bytes([tid])))

    def test_reject_unsupported_heat_requests(self):
        for tokens in (
            ["tcal", "heat"], ["tcal", "heat", "stop"],
            ["tcal", "heat", "status"], ["tcal", "heat", "44"],
            ["tcal", "heat", "start", "44"],
            ["tcal", "heat", "start", "nan"],
            ["tcal-heat-start", "44"], ["tcal-heat-start", "stop"],
        ):
            for target in ("0", "all"):
                with self.subTest(target=target, tokens=tokens):
                    with self.assertRaises(ValueError):
                        sender.build_send(target, tokens)


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(HeatedSend)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)
    with tempfile.TemporaryDirectory(prefix="receiver-remote-commands-") as directory:
        temporary = Path(directory)
        (temporary / "zephyr/sys").mkdir(parents=True)
        (temporary / "esb.h").write_text("struct esb_evt;\n")
        (temporary / "zephyr/sys/printk.h").write_text("#define printk(...) ((void)0)\n")
        binary = temporary / "console-send"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-g", "-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-fno-pie", "-no-pie", "-I", str(temporary), "-I", str(ROOT / "src"),
            str(ROOT / "src/console_send.c"), str(HERE / "test_console_send.c"),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)
