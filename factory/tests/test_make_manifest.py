import struct
import sys
import tempfile
import unittest
from pathlib import Path

FACTORY_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(FACTORY_ROOT))

from make_manifest import EXPECTED_PARTITIONS, validate_partition_table  # noqa: E402


def partition_payload(overrides=None):
    entries = dict(EXPECTED_PARTITIONS)
    if overrides:
        entries.update(overrides)
    payload = bytearray()
    for label, (part_type, subtype, offset, size) in entries.items():
        encoded = label.encode("ascii").ljust(16, b"\0")
        payload.extend(struct.pack("<HBBII16sI", 0x50AA, part_type, subtype, offset, size, encoded, 0))
    payload.extend(b"\xff" * 32)
    return bytes(payload).ljust(0xC00, b"\xff")


class PartitionManifestTests(unittest.TestCase):
    def test_expected_dual_ota_littlefs_layout_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "partitions.bin"
            path.write_bytes(partition_payload())
            validate_partition_table(path)

    def test_coredump_instead_of_littlefs_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "partitions.bin"
            payload = partition_payload({"spiffs": (0x01, 0x03, 0x3D0000, 0x20000)})
            path.write_bytes(payload)
            with self.assertRaisesRegex(RuntimeError, "spiffs"):
                validate_partition_table(path)


if __name__ == "__main__":
    unittest.main()
