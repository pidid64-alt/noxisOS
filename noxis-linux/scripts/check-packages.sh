#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# check-packages.sh — verify that every package in the profile's package list
# exists in the configured repositories. Run it before a (slow) ISO build.
#
#   ./scripts/check-packages.sh [profile-dir]
# ---------------------------------------------------------------------------
set -uo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
profile=${1:-"${script_dir}/../configs/noxis"}
profile=$(realpath -- "$profile")
list="${profile}/packages.$(uname -m)"
pacman_conf="${profile}/pacman.conf"

[[ -f $list ]]       || { echo "no package list at ${list}" >&2; exit 2; }
[[ -f $pacman_conf ]] || { echo "no pacman.conf at ${pacman_conf}" >&2; exit 2; }

echo "==> syncing databases (${pacman_conf})"
pacman --config "$pacman_conf" -Sy --noconfirm >/dev/null || {
    echo "!!! could not sync package databases" >&2
    exit 2
}

mapped=$(mktemp)
awk 'BEGIN{IGNORECASE=0}
     /^[[:space:]]*(#|$)/ { next }
     { print $0 }' "$list" | sort -u >"$mapped"

missing=()
ok=0
while read -r pkg; do
    [[ -n $pkg ]] || continue
    # This profile-local package is built from source by build.sh. When the
    # source profile has no temporary local repo configured, validate its
    # PKGBUILD; with the temporary repo, resolve it through pacman as usual.
    if [[ $pkg == noxis-calamares ]]; then
        if pacman --config "$pacman_conf" -S --print --print-format '%n' --noconfirm "$pkg" >/dev/null 2>&1; then
            (( ok++ ))
        elif [[ -f ${script_dir}/../packages/noxis-calamares/PKGBUILD ]] \
             && ! grep -q '^\[noxis-local\]' "$pacman_conf"; then
            (( ok++ ))
        elif [[ ! -f ${script_dir}/../packages/noxis-calamares/PKGBUILD ]]; then
            missing+=("$pkg (local PKGBUILD missing)")
        else
            missing+=("$pkg (not found in the temporary noxis-local repo)")
        fi
        continue
    fi
    if pacman --config "$pacman_conf" -S --print --print-format '%n' --noconfirm "$pkg" >/dev/null 2>&1; then
        (( ok++ ))
    else
        missing+=("$pkg")
    fi
done <"$mapped"
rm -f "$mapped"

printf '==> resolved %d/%d packages\n' "$ok" "$(( ok + ${#missing[@]} ))"
if (( ${#missing[@]} )); then
    printf '!!! missing (%d):\n' "${#missing[@]}"
    printf '    %s\n' "${missing[@]}"
    exit 1
fi
echo "==> all packages found"
