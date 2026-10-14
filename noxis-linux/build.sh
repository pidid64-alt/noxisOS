#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# build.sh — build the NoxisOS Cachy Edition ISO
# ---------------------------------------------------------------------------
# Works on any Arch Linux based host (including CachyOS) and inside the CI
# container. Run it as a normal user: mkarchiso unshares its own namespace.
#
#   ./build.sh                      build ./out/noxis-<date>-x86_64.iso
#   ./build.sh --clean              drop ./work first (fresh package database)
#   ./build.sh --deps               install the build dependencies and exit
#   ./build.sh --tier v3            build with v3-optimized base packages
#   ./build.sh --no-verify          skip the package pre-flight check
#   ./build.sh --help               this help
# ---------------------------------------------------------------------------
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
profile="${script_dir}/configs/noxis"
work_dir="${script_dir}/work"
out_dir="${script_dir}/out"

clean=0
verify=1
tier=""
deps_only=0

deps=(archiso git sudo squashfs-tools dosfstools mtools libisoburn syslinux grub edk2-shell zstd)

usage() { sed -n '2,14p' "$0"; }

while (($#)); do
    case "$1" in
        --clean)     clean=1 ;;
        --no-verify) verify=0 ;;
        --deps)      deps_only=1 ;;
        --tier)      tier="${2:-}"; shift ;;
        --out)       out_dir="${2:-}"; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "unknown option '$1'" >&2; usage; exit 1 ;;
    esac
    shift
done

if (( ! EUID )); then
    if [[ ${NOXIS_ALLOW_ROOT:-} == 1 ]]; then
        echo "==> warning: running as root (allowed because NOXIS_ALLOW_ROOT=1, e.g. inside CI)"
    else
        echo "refusing to run as root: mkarchiso builds unprivileged (that is a feature)" >&2
        echo "set NOXIS_ALLOW_ROOT=1 if you really mean it (containers/CI)" >&2
        exit 1
    fi
fi

echo "==> checking build dependencies"
missing=()
for pkg in "${deps[@]}"; do
    pacman -Qq "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done
if (( ${#missing[@]} )); then
    echo "    missing: ${missing[*]}"
    echo "    install them with:  sudo pacman -S --needed ${missing[*]}"
    echo "    (or run: ./build.sh --deps)"
    (( deps_only )) && exit 0
    exit 1
fi
(( deps_only )) && { echo "==> dependencies satisfied"; exit 0; }

echo "==> ensuring the pacman keyring (archlinux + cachyos)"
if ! pacman-key --list-keys F3B607488DB35A47 >/dev/null 2>&1; then
    sudo pacman-key --init
    sudo pacman-key --populate archlinux
    sudo pacman-key --recv-keys F3B607488DB35A47 --keyserver keyserver.ubuntu.com || true
    sudo pacman-key --lsign-key F3B607488DB35A47 || true
fi

if [[ -n $tier ]]; then
    "${script_dir}/scripts/switch-repo-tier.sh" "$tier"
fi

(( clean )) && { echo "==> removing ${work_dir}"; rm -rf -- "$work_dir"; }

if (( verify )); then
    echo "==> running the profile self-check"
    if command -v python3 >/dev/null && python3 -c 'import yaml' 2>/dev/null; then
        python3 "${script_dir}/scripts/self-check.py" "$profile"
    else
        echo "    (skipped: python3 with PyYAML is not installed — pacman -S python-yaml)"
    fi

    echo "==> verifying the package list"
    "${script_dir}/scripts/check-packages.sh" "$profile"
fi

echo "==> building the ISO (this takes a while)"
mkdir -p -- "$out_dir"
mkarchiso -v -w "$work_dir" -o "$out_dir" "$profile"

echo
echo "==> done. ISO(s) in ${out_dir}:"
ls -lh -- "$out_dir"
echo
echo "Try it with:  qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 \\"
echo "                -device virtio-vga-gl -display gtk,gl=on \\"
echo "                -drive file=${out_dir}/$(ls -1 "$out_dir" | grep -m1 '\.iso$'),format=raw,if=virtio"
