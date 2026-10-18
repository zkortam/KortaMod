#!/bin/sh
cd "$(dirname "$0")" || exit 1
echo "This puts ModRetro's official firmware back on your Chromatic (a backup is made first; your settings stay)."
printf 'Continue? [y/N] '
read -r answer
case "$answer" in y|Y|yes|YES) ;; *) echo 'Nothing was changed.'; read -r _; exit 0 ;; esac
./KortaMod-Installer --stock
result=$?
echo
echo 'Press Enter to close.'
read -r _
exit "$result"
