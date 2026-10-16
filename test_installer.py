from pathlib import Path
import unittest

import installer


class InstallerSafetyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.stock = Path(__file__).resolve().parents[2] / 'backups' / 'chromatic-mcu-before-kortamod-2026-10-15.bin'

    def test_package_hashes_and_app_header(self):
        app, part = installer.verify_package()
        self.assertEqual(installer.app_version(app.read_bytes()), '0.13.4-korta3')
        self.assertEqual(len(part.read_bytes()), 3072)

    def test_stock_flash_accepted(self):
        if not self.stock.exists():
            self.skipTest('private stock flash fixture is unavailable')
        data = self.stock.read_bytes()
        self.assertEqual(installer.check_image(data[0x1000:0x2000], data[0x10000:0x11000], data[0x8000:0x9000]), '0.13.4')

    def test_unknown_layout_and_version_rejected(self):
        if not self.stock.exists():
            self.skipTest('private stock flash fixture is unavailable')
        data = bytearray(self.stock.read_bytes())
        data[0x8000] ^= 1
        with self.assertRaisesRegex(RuntimeError, 'Partition layout'):
            installer.check_image(data[0x1000:0x2000], data[0x10000:0x11000], data[0x8000:0x9000])
        data[0x8000] ^= 1
        data[0x10030:0x10036] = b'9.99.9'
        with self.assertRaisesRegex(RuntimeError, 'untested'):
            installer.check_image(data[0x1000:0x2000], data[0x10000:0x11000], data[0x8000:0x9000])


if __name__ == '__main__':
    unittest.main()
