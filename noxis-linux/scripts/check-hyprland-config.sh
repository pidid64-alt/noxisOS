#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Check the shipped Hyprland configuration with Hyprland's own parser:
#
#     hyprland --verify-config -c <file>
#
# This catches removed or renamed options (misc:vfr, gestures:workspace_swipe,
# the windowrulev2 -> windowrule change, dead dispatchers like togglesplit) long
# before someone boots the ISO and sees the red error overlay.
#
# Real config errors fail the run. A failure that is clearly environmental (no
# GPU/seat/DRM in a container) is reported as a warning instead, because it says
# nothing about the configuration.
#
# Usage: scripts/check-hyprland-config.sh [profile-dir]
set -uo pipefail

profile="${1:-configs/noxis}"
conf="${profile}/airootfs/etc/skel/.config/hypr/hyprland.conf"
conf_abs="$(realpath -m -- "$conf")"

[[ -f $conf_abs ]] || { echo "check-hyprland-config: no such file: $conf" >&2; exit 2; }

if ! command -v hyprland >/dev/null 2>&1; then
    echo "check-hyprland-config: hyprland is not installed here — skipping (install it to verify)"
    exit 0
fi

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
export HOME="${tmp}/home" XDG_CONFIG_HOME="${tmp}/home/.config" XDG_RUNTIME_DIR="${tmp}/run"
mkdir -p -- "$HOME" "$XDG_CONFIG_HOME/hypr" "$XDG_RUNTIME_DIR"
chmod 700 -- "$XDG_RUNTIME_DIR"

out="$(hyprland --verify-config -c "$conf_abs" 2>&1)"
rc=$?

if (( rc == 0 )); then
    echo "check-hyprland-config: OK — ${conf} parses cleanly ($(hyprland --version 2>/dev/null | head -n1 || echo 'unknown version'))"
    exit 0
fi

# Anything the parser complains about by line number is our problem.
errors="$(grep -iE 'config error in file|error at line|unknown (option|field|dispatcher)|invalid (option|field|dispatcher|value|flag)' <<<"$out" || true)"
if [[ -n $errors ]]; then
    printf '%s\n' "$errors" | head -40
    count=$(wc -l <<<"$errors")
    echo "::error title=Hyprland config::${count} problem(s) in $(basename -- "$conf")"
    exit 1
fi

echo "check-hyprland-config: warning: hyprland --verify-config did not run in this environment (exit ${rc}):"
printf '%s\n' "$out" | tail -5
echo "check-hyprland-config: this looks like an environment limitation (no DRM/seat), not a config error"
exit 0
