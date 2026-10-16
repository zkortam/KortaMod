#!/bin/sh
cd "$(dirname "$0")" || exit 1
./KortaMod-Installer --sync-clock
result=$?
echo
if [ "$result" -eq 0 ]; then echo 'Clock synced. Press Enter to close.'; else echo 'Clock sync stopped. Read the message above. Press Enter to close.'; fi
read -r _
exit "$result"
