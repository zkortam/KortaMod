#!/bin/sh
cd "$(dirname "$0")" || exit 1
exec ./KortaMod-Installer --sync-clock
