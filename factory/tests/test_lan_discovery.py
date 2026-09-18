import json
import unittest

from factory.lan_discovery import broadcast_targets, parse_response


NONCE = "0123456789abcdef"


def response(**changes):
    payload = {
        "protocol": 1,
        "product": "sms-forwarder-board",
        "nonce": NONCE,
        "firmware": "sukiyra-1.6.0",
        "ip": "192.168.31.8",
        "port": 80,
        "mac": "80:53:E0:B7:69:D8",
        "chipId": "D869B7E05380",
        "modem": "ML307A",
        "ota": True,
        "apMode": False,
    }
    payload.update(changes)
    return json.dumps(payload).encode()


class LanDiscoveryTests(unittest.TestCase):
    def test_valid_response(self):
        device = parse_response(response(), "192.168.31.8", NONCE)
        self.assertEqual(device.identity, "D869B7E05380")
        self.assertEqual(device.url, "http://192.168.31.8/")
        self.assertTrue(device.ota)

    def test_nonce_product_and_source_are_verified(self):
        with self.assertRaises(ValueError):
            parse_response(response(nonce="fedcba9876543210"), "192.168.31.8", NONCE)
        with self.assertRaises(ValueError):
            parse_response(response(product="other-device"), "192.168.31.8", NONCE)
        with self.assertRaises(ValueError):
            parse_response(response(), "192.168.31.9", NONCE)

    def test_public_and_malformed_identity_are_rejected(self):
        with self.assertRaises(ValueError):
            parse_response(response(ip="8.8.8.8"), "8.8.8.8", NONCE)
        with self.assertRaises(ValueError):
            parse_response(response(mac="not-a-mac"), "192.168.31.8", NONCE)
        with self.assertRaises(ValueError):
            parse_response(response(chipId="1234"), "192.168.31.8", NONCE)

    def test_broadcast_targets_include_limited_and_local_24(self):
        self.assertEqual(
            broadcast_targets("192.168.31.19"),
            ("255.255.255.255", "192.168.31.255"),
        )


if __name__ == "__main__":
    unittest.main()
