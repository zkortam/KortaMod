# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_all

tool_data, tool_binaries, tool_hidden = collect_all('esptool')
serial_data, serial_binaries, serial_hidden = collect_all('serial')
a = Analysis(
    ['installer.py'],
    pathex=[],
    binaries=tool_binaries + serial_binaries,
    datas=tool_data + serial_data + [
        ('firmware/mcu_fw.bin', 'firmware'),
        ('firmware/partition-table.bin', 'firmware'),
    ],
    hiddenimports=tool_hidden + serial_hidden,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='KortaMod-Installer',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    console=True,
)
