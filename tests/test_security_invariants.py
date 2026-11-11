#!/usr/bin/env python3
"""Security and hygiene regression tests for ESP32-S3-CAM-DHT."""

from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parent.parent


class TestSecurityInvariants(unittest.TestCase):
    def test_local_properties_not_tracked(self) -> None:
        tracked = subprocess.check_output(["git", "ls-files"], cwd=ROOT, text=True).splitlines()
        self.assertNotIn("android-app/local.properties", tracked)

    def test_firmware_enforces_auth_and_path_validation(self) -> None:
        fw = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertNotIn("admin123", fw)
        self.assertNotIn('"changeme"', fw)
        self.assertIn("camsec::isValidSafeSegment", fw)
        self.assertIn("camsec::constantTimeEquals", fw)
        self.assertIn('gServer.collectHeaders(kCollectedHeaders, 1)', fw)
        self.assertNotIn("goto readings", fw)


if __name__ == "__main__":
    unittest.main()
