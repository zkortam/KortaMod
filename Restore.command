#!/bin/sh
cd "$(dirname "$0")" || exit 1
backup_file=$(osascript -e 'POSIX path of (choose file with prompt "Choose this Chromatic’s KortaMod backup")') || exit 1
./KortaMod-Installer --restore "$backup_file"
result=$?
echo
echo 'Press Enter to close.'
read -r _
exit "$result"
