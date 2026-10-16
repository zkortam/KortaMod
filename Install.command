#!/bin/sh
cd "$(dirname "$0")" || exit 1
./KortaMod-Installer
result=$?
echo
if [ "$result" -eq 0 ]; then echo 'Install complete. Press Enter to close.'; else echo 'Install stopped. Read the message above. Press Enter to close.'; fi
read -r _
exit "$result"
