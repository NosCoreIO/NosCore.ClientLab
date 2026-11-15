import base64
import json
import pathlib
import tempfile
import unittest
from unittest.mock import patch, MagicMock

from gameforge_identity import OfficialIdentity, encode_blackbox, decode_blackbox


class OfficialIdentityTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.filename = pathlib.Path(self.directory.name) / "identity.json"
        self.fields = [None] * 30
        self.fields[0] = 12
        self.fields[7] = ["en-GB", "日本語"]
        self.fields[23] = 251
        self.fields[25] = base64.b64encode(b"test vector 1000").decode()
        self.fields[26] = "captured browser UA"
        self.fields[29] = 8192
        self.write_identity()

    def write_identity(self):
        self.filename.write_text(json.dumps({"installationId": "installation", "fingerprint": self.fields}), encoding="utf-8")

    def test_codec_preserves_unicode_and_nested_request(self):
        self.fields[28] = {"session": "session", "features": [4294967293]}
        self.assertEqual(self.fields, decode_blackbox(encode_blackbox(self.fields)))

    def test_refresh_preserves_collected_fields_and_binds_request(self):
        identity = OfficialIdentity(self.filename, "installation")
        response = MagicMock()
        response.__enter__.return_value.headers = {"Date": "Sun, 15 Nov 2026 01:00:00 GMT"}
        request = {"session": "new-session", "installation": "installation", "features": [1]}
        with patch("gameforge_identity.urllib.request.urlopen", return_value=response):
            result = decode_blackbox(identity.blackbox(request))
            subsequent = decode_blackbox(identity.blackbox())
        for index in set(range(30)) - {21, 25, 27, 28}:
            self.assertEqual(self.fields[index], result[index])
        self.assertEqual(request, result[28])
        self.assertIsNone(subsequent[28])
        self.assertEqual("2026-11-15T01:00:00.000Z", result[27])
        self.assertNotEqual(self.fields[25], result[25])

    def test_wrong_installation_is_rejected(self):
        with self.assertRaises(ValueError):
            OfficialIdentity(self.filename, "different-installation")

    def test_unsupported_schema_is_rejected(self):
        self.fields[0] = 11
        self.write_identity()
        with self.assertRaises(ValueError):
            OfficialIdentity(self.filename, "installation")


if __name__ == "__main__":
    unittest.main()
