#!/usr/bin/env python3
"""Offline regression checks for the release-aware OpenPS2BIOS bootstrap."""

import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import ensure_openps2bios as updater


class TestOpenPS2BIOSBootstrap(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="rom0-latest-")
        self.addCleanup(self.temp.cleanup)
        self.cache = Path(self.temp.name)
        self.image = b"A" * updater.ROM0_SIZE
        self.digest = hashlib.sha256(self.image).hexdigest()

    @staticmethod
    def release(version, release_id, asset_id):
        name = f"OpenPS2BIOS-{version}.bin"
        root = (
            "https://github.com/TamaribaEmu/OpenPS2BIOS/"
            f"releases/download/v{version}/"
        )
        return {
            "tag_name": f"v{version}",
            "id": release_id,
            "draft": False,
            "prerelease": False,
            "assets": [
                {
                    "name": name,
                    "id": asset_id,
                    "size": updater.ROM0_SIZE,
                    "browser_download_url": root + name,
                },
                {
                    "name": name + ".sha256",
                    "id": asset_id + 1,
                    "size": 90,
                    "browser_download_url": root + name + ".sha256",
                },
            ],
        }

    def download(self, url, limit):
        if url.endswith(".sha256"):
            filename = url.rsplit("/", 1)[-1][:-7]
            return f"{self.digest}  {filename}\n".encode("ascii")
        return self.image

    def test_download_cache_and_upgrade(self):
        releases = [
            self.release("0.6", 6, 60),
            self.release("0.6", 6, 60),
            self.release("0.7", 7, 70),
        ]
        with mock.patch.object(
            updater, "fetch_latest_release", side_effect=releases
        ), mock.patch.object(
            updater, "download_bytes", side_effect=self.download
        ) as download:
            path = updater.ensure_rom0(self.cache)
            self.assertEqual(path.read_bytes(), self.image)
            self.assertEqual(download.call_count, 2)
            cached = updater.ensure_rom0(self.cache)
            self.assertEqual(cached, path)
            self.assertEqual(download.call_count, 2)
            upgraded = updater.ensure_rom0(self.cache)
            self.assertEqual(upgraded, path)
            self.assertEqual(download.call_count, 4)
            self.assertEqual(updater.validated_cache(self.cache)["tag_name"],
                             "v0.7")

    def test_checksum_rejection_and_offline_cache_fallback(self):
        release = self.release("0.6", 6, 60)
        with mock.patch.object(
            updater, "fetch_latest_release", return_value=release
        ), mock.patch.object(
            updater, "download_bytes",
            side_effect=lambda url, limit:
                b"0" * 64 if url.endswith(".sha256") else self.image
        ):
            with self.assertRaises(ValueError):
                updater.ensure_rom0(self.cache)
        self.assertFalse((self.cache / updater.ROM0_NAME).exists())

        with mock.patch.object(
            updater, "fetch_latest_release", return_value=release
        ), mock.patch.object(
            updater, "download_bytes", side_effect=self.download
        ):
            cached = updater.ensure_rom0(self.cache)
        self.assertEqual(cached.stat().st_size, updater.ROM0_SIZE)

        with mock.patch.object(
            updater, "fetch_latest_release", side_effect=OSError("offline")
        ):
            self.assertEqual(updater.ensure_rom0(self.cache), cached)

        cached.write_bytes(b"corrupted")
        with mock.patch.object(
            updater, "fetch_latest_release", side_effect=OSError("offline")
        ):
            with self.assertRaises(RuntimeError):
                updater.ensure_rom0(self.cache)

    def test_fail_closed_on_invalid_release_or_path(self):
        bad_release = self.release("0.6", 6, 60)
        bad_release["assets"][0]["size"] = 1024
        with self.assertRaises(ValueError):
            updater.select_release_assets(bad_release)
        bad_release["assets"][0]["size"] = updater.ROM0_SIZE
        bad_release["assets"][0]["browser_download_url"] = (
            "https://attacker.example/rom0.bin"
        )
        with self.assertRaises(ValueError):
            updater.select_release_assets(bad_release)
        with self.assertRaises(ValueError):
            updater.parse_sha256(b"0" * 64 + b"  wrong.bin\n",
                                 "OpenPS2BIOS-0.6.bin")

    def test_batch_keeps_manual_override_and_irx_mode(self):
        script = (
            Path(__file__).resolve().parents[2] / "scripts" / "run_sdbz_native.bat"
        ).read_text(encoding="utf-8")
        self.assertIn(
            "if not defined SDBZ_IOP_ROM0 if not defined IOP_IRX "
            "if not defined IOP_IRX_LIST (",
            script,
        )
        self.assertIn(
            'set "SDBZ_IOP_ROM0=%CD%\\build\\openps2bios\\rom0.bin"',
            script,
        )
        self.assertIn("python tools\\ensure_openps2bios.py", script)
        self.assertIn('if not exist "%SDBZ_IOP_ROM0%" (', script)


if __name__ == "__main__":
    unittest.main()
