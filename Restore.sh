#!/bin/sh
cd "$(dirname "$0")" || exit 1
if [ -z "$1" ]; then
  echo 'Usage: ./Restore.sh path/to/your/chromatic-backup.bin'
  exit 1
fi
exec ./KortaMod-Installer --restore "$1"
