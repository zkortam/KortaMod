from pathlib import Path
import json
import hashlib
import os
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

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

    def test_inspect_uses_one_flash_read(self):
        app, part = installer.verify_package()
        initial = bytearray(b'\xff' * 0x10000)
        initial[0] = 0xe9
        initial[0x7000:0x7000 + part.stat().st_size] = part.read_bytes()
        initial[0xf000:0x10000] = app.read_bytes()[:0x1000]
        with patch.object(installer, 'read_region', return_value=bytes(initial)) as read:
            self.assertEqual(installer.inspect('test-port'), '0.13.4-korta3')
            read.assert_called_once_with('test-port', 0x1000, 0x10000)

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

    def test_flash_command_uses_expected_chip_and_port(self):
        calls = []
        fake = SimpleNamespace(main=lambda args: calls.append(args))
        with patch.dict(sys.modules, {'esptool': fake}):
            installer.esptool_command('test-port', 'read_flash', '0x0', '0x1000', 'test.bin')
        self.assertEqual(calls, [['--chip', 'esp32', '--port', 'test-port', '--baud', '460800', 'read_flash', '0x0', '0x1000', 'test.bin']])

    def synthetic_flash(self):
        app, part = installer.verify_package()
        data = bytearray(b'\xff' * installer.FLASH_BYTES)
        data[0x1000] = 0xe9
        data[installer.PART_OFFSET:installer.PART_OFFSET + part.stat().st_size] = part.read_bytes()
        data[installer.APP_OFFSET:installer.APP_OFFSET + app.stat().st_size] = app.read_bytes()
        return bytes(data)

    def test_backup_is_private_and_bound_to_device(self):
        data = self.synthetic_flash()
        with tempfile.TemporaryDirectory() as directory:
            def fake_esptool(port, command, start, size, path):
                self.assertEqual(command, 'read_flash')
                Path(path).write_bytes(data)
            with patch.object(installer, 'backup_directory', return_value=Path(directory)), \
                 patch.object(installer, 'device_mac', return_value='aa:bb:cc:dd:ee:ff'), \
                 patch.object(installer, 'esptool_command', side_effect=fake_esptool):
                image = installer.backup('test-port')
            record = json.loads(image.with_suffix('.json').read_text())
            self.assertEqual(record['mac'], 'aa:bb:cc:dd:ee:ff')
            self.assertEqual(record['sha256'], hashlib.sha256(data).hexdigest())
            if os.name != 'nt':
                self.assertEqual(image.stat().st_mode & 0o777, 0o600)
                self.assertEqual(image.with_suffix('.json').stat().st_mode & 0o777, 0o600)
            with patch.object(installer, 'require_port', return_value='test-port'), \
                 patch.object(installer, 'device_mac', return_value='00:11:22:33:44:55'), \
                 patch.object(installer, 'esptool_command') as command:
                with self.assertRaisesRegex(RuntimeError, 'different Chromatic'):
                    installer.restore(None, image)
                command.assert_not_called()

    def test_legacy_backup_requires_explicit_opt_in(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'old.bin'
            image.write_bytes(self.synthetic_flash())
            with patch.object(installer, 'require_port', return_value='test-port'), \
                 patch.object(installer, 'esptool_command') as command:
                with self.assertRaisesRegex(RuntimeError, 'identity file is missing'):
                    installer.restore(None, image)
                command.assert_not_called()


if __name__ == '__main__':
    unittest.main()
