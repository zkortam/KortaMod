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
        self.assertEqual(installer.app_version(app.read_bytes()), '0.13.4-korta8')
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
            self.assertEqual(installer.inspect('test-port'), '0.13.4-korta8')
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

    def official_image(self):
        """ModRetro's v0.13.4.bin rebuilt from a stock flash: bootloader, table, app."""
        if not self.stock.exists():
            self.skipTest('private stock flash fixture is unavailable')
        data = self.stock.read_bytes()
        image = bytearray(b'\xff' * (0x10000 + 732240))
        image[0x1000:0x9000] = data[0x1000:0x9000]
        image[0x10000:] = data[0x10000:0x10000 + 732240]
        return bytes(image)

    def test_return_to_stock_writes_official_app_and_layout(self):
        image = self.official_image()
        self.assertEqual(hashlib.sha256(image).hexdigest(), installer.STOCK_FILE_SHA256)
        calls = []
        with patch.object(installer, 'require_port', return_value='test-port'), \
             patch.object(installer, 'inspect', return_value='0.13.4-korta8'), \
             patch.object(installer, 'backup', return_value=Path('/tmp/backup.bin')), \
             patch.object(installer, 'esptool_command', side_effect=lambda port, *a: calls.append(a) or ''):
            installer.return_to_stock(None, fetch=lambda url: image)
        ops = [c[0] for c in calls]
        self.assertEqual(ops, ['erase_region', 'erase_region', 'write_flash', 'verify_flash'])
        self.assertEqual(calls[0][1:], (hex(0x110000), hex(0x190000)), 'KortaMod areas cleared')
        self.assertEqual(calls[1][1:], (hex(0x10000), hex(0x100000)), 'whole app partition cleared')
        self.assertEqual(calls[2][1], hex(0x8000))
        self.assertEqual(calls[2][3], hex(0x10000))
        for c in calls:
            self.assertNotIn(hex(0x9000), c, 'settings (NVS) are never touched')

    def test_return_to_stock_refuses_a_bad_download_before_touching_the_device(self):
        touched = []
        with patch.object(installer, 'require_port', side_effect=lambda *a: touched.append('port')), \
             patch.object(installer, 'esptool_command', side_effect=lambda *a: touched.append('flash')):
            with self.assertRaisesRegex(RuntimeError, 'checksum'):
                installer.return_to_stock(None, fetch=lambda url: b'\xe9' * 1000)
            def offline(url):
                raise OSError('offline')
            with self.assertRaisesRegex(RuntimeError, 'download'):
                installer.return_to_stock(None, fetch=offline)
        self.assertEqual(touched, [])

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


class RecoveryTests(unittest.TestCase):
    def setUp(self):
        import tempfile
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.identity = '11:22:33:44:55:66'
        app, part = installer.verify_package()
        self.data = bytearray(b'\xff' * installer.FLASH_BYTES)
        self.data[0x1000] = 0xe9
        self.data[installer.APP_OFFSET:installer.APP_OFFSET + app.stat().st_size] = app.read_bytes()
        self.data[installer.PART_OFFSET:installer.PART_OFFSET + part.stat().st_size] = part.read_bytes()

    def run_backup(self, incomplete=False):
        def flash(port, *args):
            if args[0] == 'read_flash':
                Path(args[-1]).write_bytes(self.data[:32] if incomplete else self.data)
        with patch.object(installer, 'backup_directory', return_value=self.directory), \
             patch.object(installer, 'device_mac', return_value=self.identity), \
             patch.object(installer, 'esptool_command', side_effect=flash):
            return installer.backup('test-port')

    def test_backup_manifest_and_private_permissions(self):
        import json
        import hashlib
        import os
        image = self.run_backup()
        record = json.loads(image.with_suffix('.json').read_text())
        self.assertEqual(record, {'mac': self.identity, 'sha256': hashlib.sha256(self.data).hexdigest()})
        if os.name != 'nt':
            self.assertEqual(image.stat().st_mode & 0o777, 0o600)

    def test_incomplete_backup_removed(self):
        with self.assertRaisesRegex(RuntimeError, 'incomplete'):
            self.run_backup(incomplete=True)
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_damaged_and_foreign_restore_never_writes(self):
        image = self.run_backup()
        with patch.object(installer, 'require_port', return_value='test-port'), \
             patch.object(installer, 'device_mac', return_value='ff:ff:ff:ff:ff:ff'), \
             patch.object(installer, 'esptool_command') as flash:
            with self.assertRaisesRegex(RuntimeError, 'different Chromatic'):
                installer.restore(None, image)
            flash.assert_not_called()
        damaged = bytearray(image.read_bytes())
        damaged[-1] ^= 1
        image.write_bytes(damaged)
        with patch.object(installer, 'require_port', return_value='test-port'), \
             patch.object(installer, 'esptool_command') as flash:
            with self.assertRaisesRegex(RuntimeError, 'checksum'):
                installer.restore(None, image)
            flash.assert_not_called()

    def test_valid_restore_writes_and_verifies(self):
        image = self.run_backup()
        with patch.object(installer, 'require_port', return_value='test-port'), \
             patch.object(installer, 'device_mac', return_value=self.identity), \
             patch.object(installer, 'esptool_command') as flash:
            installer.restore(None, image)
            self.assertEqual([call.args[1] for call in flash.call_args_list], ['write_flash', 'verify_flash'])

    def test_compatibility_errors_without_private_fixture(self):
        data = self.data
        data[installer.PART_OFFSET] ^= 1
        with self.assertRaisesRegex(RuntimeError, 'Partition layout'):
            installer.check_image(data[0x1000:0x2000], data[0x10000:0x11000], data[0x8000:0x9000])
        data[installer.PART_OFFSET] ^= 1
        data[0x10030:0x10050] = b'9.99.9'.ljust(32, b'\0')
        with self.assertRaisesRegex(RuntimeError, 'untested'):
            installer.check_image(data[0x1000:0x2000], data[0x10000:0x11000], data[0x8000:0x9000])

    def run_install(self, current_layout):
        calls = []
        with patch.object(installer, 'require_port', return_value='test-port'), \
             patch.object(installer, 'inspect', side_effect=lambda p: calls.append(('inspect',))), \
             patch.object(installer, 'backup', side_effect=lambda p: calls.append(('backup',)) or Path('saved.bin')), \
             patch.object(installer, 'backup_layout', return_value=current_layout), \
             patch.object(installer, 'sync_clock', side_effect=lambda p: calls.append(('sync_clock',))), \
             patch.object(installer, 'esptool_command', side_effect=lambda p, *a: calls.append(a)):
            installer.install(None)
        return calls

    def test_install_order_and_backup_failure_prevents_write(self):
        calls = self.run_install('korta')
        self.assertEqual([c[0] for c in calls], ['inspect', 'backup', 'erase_region', 'write_flash', 'verify_flash', 'sync_clock'])
        self.assertEqual(calls[2][1:], ('0x110000', '0x2000'), 'otadata cleared so factory boots')
        self.assertEqual(calls[3][1], hex(installer.APP_OFFSET), 'games and the table are left alone')
        with patch.object(installer, 'require_port', return_value='test-port'), \
             patch.object(installer, 'inspect'), \
             patch.object(installer, 'backup', side_effect=RuntimeError('backup failed')), \
             patch.object(installer, 'esptool_command') as flash:
            with self.assertRaisesRegex(RuntimeError, 'backup failed'):
                installer.install(None)
            flash.assert_not_called()

class LayoutTests(unittest.TestCase):
    def table(self, name):
        return installer.resource(name).read_bytes()

    def test_both_layouts_recognised(self):
        self.assertEqual(installer.layout(self.table('partition-table.bin')), 'stock')
        self.assertEqual(installer.layout(self.table('partition-table-korta.bin')), 'korta')
        other = bytearray(self.table('partition-table-korta.bin'))
        other[40] ^= 1
        with self.assertRaisesRegex(RuntimeError, 'Partition layout'):
            installer.layout(bytes(other))

    def test_korta_table_keeps_stock_entries_and_adds_slots(self):
        import struct
        def entries(blob):
            out = []
            for i in range(0, len(blob), 32):
                e = blob[i:i + 32]
                if e[:2] != b'\xaa\x50':
                    break
                out.append((e[12:28].split(b'\0')[0].decode(), e[2], e[3], *struct.unpack('<II', e[4:12])))
            return out
        stock, korta = entries(self.table('partition-table.bin')), entries(self.table('partition-table-korta.bin'))
        self.assertEqual(korta[:len(stock)], stock, 'nvs, phy_init and factory are unchanged')
        self.assertEqual(korta[len(stock):], [('otadata', 1, 0, 0x110000, 0x2000), ('ota_0', 0, 0x10, 0x120000, 0x100000),
                                              ('games', 1, 0x40, 0x220000, 0x80000)])
        self.assertEqual((installer.OTADATA, installer.GAMES), ((0x110000, 0x2000), (0x220000, 0x80000)))

    def test_versions(self):
        for ok in ('0.13.4', '0.13.4-korta3', '0.13.4-korta8', '0.13.4-korta12'):
            self.assertTrue(installer.VERSION_RE.match(ok), ok)
        for bad in ('0.13.5', '0.13.4-korta1', '0.13.4-kortax', '9.99.9'):
            self.assertFalse(installer.VERSION_RE.match(bad), bad)

    def test_stock_layout_is_migrated_after_backup(self):
        calls = RecoveryTests.run_install(self, 'stock')
        self.assertEqual([c[0] for c in calls],
                         ['inspect', 'backup', 'erase_region', 'erase_region', 'write_flash', 'verify_flash', 'sync_clock'])
        self.assertEqual(calls[3][1:], ('0x220000', '0x80000'))
        self.assertEqual(calls[4][1], hex(installer.PART_OFFSET))
        self.assertTrue(calls[4][2].endswith('partition-table-korta.bin'))
        self.assertEqual(calls[4][3], hex(installer.APP_OFFSET))
        self.assertEqual(calls[5][1:], calls[4][1:], 'both table and app are verified')

    def test_backup_layout_reads_the_saved_image(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'b.bin'
            data = bytearray(b'\xff' * 0x10000)
            data[installer.PART_OFFSET:installer.PART_OFFSET + 3072] = self.table('partition-table.bin')
            image.write_bytes(bytes(data))
            self.assertEqual(installer.backup_layout(image), 'stock')

    def test_korta_layout_flash_passes_checks(self):
        app, _ = installer.verify_package()
        self.assertEqual(installer.check_image(b'\xe9' + b'\0' * 0x1f, app.read_bytes()[:0x1000],
                                               self.table('partition-table-korta.bin')), installer.app_version(app.read_bytes()))


if __name__ == '__main__':
    unittest.main()
