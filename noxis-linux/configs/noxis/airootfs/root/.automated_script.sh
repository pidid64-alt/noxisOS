#!/usr/bin/env bash
# Executed automatically by the live system when the kernel command line
# contains "script=<path>" (archiso feature). Useful for automated tests.
set -euo pipefail

script_url="${script_url:-}"
[[ -n "$script_url" ]] || { echo "usage: boot with script_url=<url>"; exit 0; }

curl -fsSL "$script_url" -o /tmp/automated.sh
chmod +x /tmp/automated.sh
/tmp/automated.sh
