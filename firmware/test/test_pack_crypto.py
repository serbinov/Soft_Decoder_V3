import importlib.util
from pathlib import Path
import unittest

PACK = Path(__file__).resolve().parents[1] / "tools" / "pack_sounds.py"
SPEC = importlib.util.spec_from_file_location("pack_sounds_crypto", PACK)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class AspCryptoTests(unittest.TestCase):
    def test_authenticated_roundtrip_and_format(self):
        plain = b"AURASP01" + bytes(range(256)) * 8
        first = MODULE.encrypt_asp(plain, "correct horse")
        second = MODULE.encrypt_asp(plain, "correct horse")
        self.assertTrue(first.startswith(b"AURAPW02"))
        self.assertEqual(first[:4], b"AURA")
        self.assertEqual(first[8], 2)       # format version
        self.assertEqual(first[9], 2)       # scrypt
        self.assertEqual(first[10], 1)      # ChaCha20
        self.assertEqual(first[11], 1)      # HMAC-SHA256
        self.assertEqual(first[12], MODULE.SCRYPT_LOG_N)
        self.assertEqual(first[13], MODULE.SCRYPT_R)
        self.assertEqual(first[14], MODULE.SCRYPT_P)
        self.assertNotEqual(first[16:32], second[16:32], "salt must be random")
        self.assertNotEqual(first[32:44], second[32:44], "nonce must be random")
        self.assertEqual(MODULE.decrypt_asp(first, "correct horse"), plain)

    def test_wrong_password_and_tampering_are_rejected(self):
        plain = b"AURASP01" + b"payload" * 100
        enc = MODULE.encrypt_asp(plain, "correct horse")
        with self.assertRaises(SystemExit):
            MODULE.decrypt_asp(enc, "wrong horse")
        for offset in (12, 20, MODULE.HDR2_LEN + 3, len(enc) - 1):
            tampered = bytearray(enc)
            tampered[offset] ^= 0x01
            with self.assertRaises(SystemExit, msg="tamper offset %d" % offset):
                MODULE.decrypt_asp(bytes(tampered), "correct horse")
        for length in (8, MODULE.HDR2_LEN, len(enc) - 1):
            with self.assertRaises(SystemExit):
                MODULE.decrypt_asp(enc[:length], "correct horse")

    def test_kdf_parameters_are_bounded(self):
        enc = bytearray(MODULE.encrypt_asp(b"data", "correct horse"))
        for index, value in ((12, 20), (13, 1), (14, 99)):
            broken = bytearray(enc)
            broken[index] = value
            with self.assertRaises(SystemExit):
                MODULE.decrypt_asp(bytes(broken), "correct horse")

    def test_password_minimum_for_new_packs(self):
        with self.assertRaises(ValueError):
            MODULE.encrypt_asp(b"data", "short")

    def test_legacy_aurapw01_still_imports(self):
        plain = b"AURASP01legacy-payload"
        legacy = MODULE._encrypt_legacy(plain, "old")
        self.assertTrue(legacy.startswith(b"AURAPW01"))
        self.assertEqual(MODULE.decrypt_asp(legacy, "old"), plain)
        with self.assertRaises(SystemExit):
            MODULE.decrypt_asp(legacy, "wrong")


if __name__ == "__main__":
    unittest.main()
