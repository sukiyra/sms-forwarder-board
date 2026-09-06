import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

FACTORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(FACTORY_ROOT))

from production_tool import (  # noqa: E402
    FirmwareBundle,
    ReleaseVersion,
    evaluate_status,
    install_release_archive,
    parse_release_versions,
    save_record,
)


def ready_status(family="ML307Y"):
    return {
        "firmware": "sukiyra-test",
        "chip": {"model": "ESP32-C3", "id": "AABBCCDDEEFF"},
        "modem": {
            "supported": True,
            "family": family,
            "model": family + "-TEST",
            "firmware": "TEST",
            "smsMode": "direct",
            "registered": True,
        },
        "sim": {"ready": True, "smsReady": True, "type": "physical", "iccidTail": "1234"},
        "network": {"plmn": "46001"},
    }


class ProductionToolTests(unittest.TestCase):
    def test_supported_modem_families_pass(self):
        for family in ("ML307A", "ML307C", "ML307R", "ML307Y"):
            with self.subTest(family=family):
                self.assertEqual(evaluate_status(ready_status(family), True, True), [])

    def test_requirements_are_enforced(self):
        status = ready_status()
        status["sim"]["ready"] = False
        status["modem"]["registered"] = False
        failures = evaluate_status(status, True, True)
        self.assertIn("SIM 未就绪", failures)
        self.assertIn("未完成蜂窝网络注册", failures)

    def test_manifest_hash_is_verified(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            image = root / "app.bin"
            image.write_bytes(b"firmware")
            import hashlib
            manifest = {
                "chip": "esp32c3",
                "version": "test",
                "images": [{"offset": "0x10000", "file": "app.bin", "sha256": hashlib.sha256(b"firmware").hexdigest()}],
            }
            (root / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            self.assertEqual(FirmwareBundle.load(root).version, "test")
            image.write_bytes(b"tampered")
            with self.assertRaisesRegex(RuntimeError, "校验失败"):
                FirmwareBundle.load(root)

    def test_manifest_cannot_escape_bundle_directory(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            bundle = root / "bundle"
            bundle.mkdir()
            (root / "outside.bin").write_bytes(b"outside")
            manifest = {
                "chip": "esp32c3",
                "version": "test",
                "images": [{"offset": "0x10000", "file": "../outside.bin"}],
            }
            (bundle / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "不安全路径"):
                FirmwareBundle.load(bundle)

    def test_csv_and_json_records_are_written(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            save_record(root, "COM9", ready_status(), "PASS", "")
            self.assertTrue((root / "production.csv").is_file())
            details = list(root.glob("*.json"))
            self.assertEqual(len(details), 1)
            self.assertEqual(json.loads(details[0].read_text(encoding="utf-8"))["result"], "PASS")

    def test_github_release_versions_require_firmware_zip(self):
        payload = [
            {
                "tag_name": "v1.3.0",
                "name": "Version 1.3.0",
                "draft": False,
                "prerelease": False,
                "published_at": "2026-09-06T00:00:00Z",
                "assets": [
                    {
                        "name": "sms-forwarder-firmware-v1.3.0.zip",
                        "browser_download_url": "https://github.com/sukiyra/sms-forwarder-board/releases/download/v1.3.0/firmware.zip",
                    }
                ],
            },
            {"tag_name": "draft", "draft": True, "assets": []},
        ]
        versions = parse_release_versions(payload)
        self.assertEqual([item.tag for item in versions], ["v1.3.0"])
        self.assertEqual(versions[0].label, "v1.3.0")

    def test_release_archive_is_safe_and_version_checked(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            image_data = b"firmware"
            import hashlib
            manifest = {
                "chip": "esp32c3",
                "version": "sukiyra-1.3.0",
                "images": [
                    {
                        "offset": "0x10000",
                        "file": "app.bin",
                        "sha256": hashlib.sha256(image_data).hexdigest(),
                    }
                ],
            }
            archive = root / "firmware.zip"
            with zipfile.ZipFile(archive, "w") as package:
                package.writestr("bundle/manifest.json", json.dumps(manifest))
                package.writestr("bundle/app.bin", image_data)
            release = ReleaseVersion("v1.3.0", "v1.3.0", archive.name, "https://github.com/test/firmware.zip", "")
            bundle = install_release_archive(archive, root / "cache", release)
            self.assertEqual(bundle.version, "sukiyra-1.3.0")

            unsafe = root / "unsafe.zip"
            with zipfile.ZipFile(unsafe, "w") as package:
                package.writestr("../escape.bin", b"bad")
            with self.assertRaisesRegex(RuntimeError, "不安全路径"):
                install_release_archive(unsafe, root / "unsafe-cache", release)


if __name__ == "__main__":
    unittest.main()
