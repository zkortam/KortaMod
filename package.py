"""Make a double-clickable ZIP from a platform's PyInstaller executable."""
from pathlib import Path
import sys
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

platform = sys.argv[1]
executable = 'KortaMod-Installer.exe' if platform == 'Windows' else 'KortaMod-Installer'
launcher = {'Windows': 'Install.bat', 'macOS-AppleSilicon': 'Install.command', 'macOS-Intel': 'Install.command', 'Linux': 'Install.sh'}[platform]
restore = {'Windows': 'Restore.bat', 'macOS-AppleSilicon': 'Restore.command', 'macOS-Intel': 'Restore.command', 'Linux': 'Restore.sh'}[platform]
clock = {'Windows': 'SyncClock.bat', 'macOS-AppleSilicon': 'SyncClock.command', 'macOS-Intel': 'SyncClock.command', 'Linux': 'SyncClock.sh'}[platform]
stock = {'Windows': 'ReturnToStock.bat', 'macOS-AppleSilicon': 'ReturnToStock.command', 'macOS-Intel': 'ReturnToStock.command', 'Linux': 'ReturnToStock.sh'}[platform]
archive = Path('dist') / f'KortaMod-{platform}.zip'
with ZipFile(archive, 'w', ZIP_DEFLATED) as output:
    for path in (Path('dist') / executable, Path(launcher), Path(restore), Path(clock), Path(stock), Path('README.md'), Path('LICENSE')):
        info = ZipInfo(path.name)
        info.compress_type = ZIP_DEFLATED
        info.create_system = 3
        info.external_attr = ((0o755 if path.name in (executable, launcher, restore, clock, stock) else 0o644) << 16)
        output.writestr(info, path.read_bytes())
print(archive)
