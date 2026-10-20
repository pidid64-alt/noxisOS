#!/usr/bin/env bash
# Build a local Calamares package against the same rolling Boost ABI shipped
# in the image, then create a local pacman repo and temporary profile copy.
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root_dir=$(cd -- "${script_dir}/.." && pwd)
source_profile="${root_dir}/configs/noxis"
repo_dir=${1:?usage: build-calamares.sh <repo-dir> [profile-copy-dir]}
profile_copy=${2:-}
repo_dir=$(realpath -m -- "$repo_dir")

packages=(
  base-devel boost boost-libs cmake doxygen extra-cmake-modules git ninja
  pacman-contrib python-jsonschema python-pyaml python-unidecode qt6-tools
  ckbcomp cryptsetup dmidecode gptfdisk hwinfo kconfig kcoreaddons kcrash
  ki18n kparts kpmcore kservice kwidgetsaddons libpwquality mkinitcpio-openswap
  networkmanager polkit-qt6 python qt6-declarative qt6-svg rsync solid
  squashfs-tools upower yaml-cpp qt6-imageformats python-toml
)
run_pacman() {
    if (( EUID == 0 )); then
        pacman --config "${source_profile}/pacman.conf" "$@"
    else
        sudo pacman --config "${source_profile}/pacman.conf" "$@"
    fi
}

# Synchronize and install the build toolchain plus Boost from the *same profile
# repositories* pacstrap will use. Cache the exact Boost packages into the local
# repo so the image cannot race a repository SONAME update during ISO assembly.
run_pacman -Sy --needed --noconfirm "${packages[@]}"
mkdir -p "$repo_dir/cache" "$repo_dir/repo"
rm -f "$repo_dir/repo/"*
run_pacman --cachedir "$repo_dir/cache" -Sw --noconfirm boost boost-libs

build_dir="${repo_dir}/build"
mkdir -p "$build_dir"
cp "${root_dir}/packages/noxis-calamares/PKGBUILD" "$build_dir/PKGBUILD"

if (( EUID == 0 )); then
    builder=noxisbuild
    if ! id "$builder" >/dev/null 2>&1; then
        useradd --create-home --shell /bin/bash "$builder"
    fi
    chown -R "$builder:$builder" "$repo_dir"
    builder_home=$(getent passwd "$builder" | cut -d: -f6)
    runuser -u "$builder" -- env HOME="$builder_home" bash -lc \
        "cd '$build_dir' && makepkg --noconfirm --cleanbuild --nodeps"
else
    (cd "$build_dir" && makepkg --noconfirm --cleanbuild --nodeps)
fi

pkg=$(find "$build_dir" -maxdepth 1 -type f -name 'noxis-calamares-[0-9]*.pkg.tar.*' -print -quit)
[[ -n $pkg ]] || { echo "build-calamares: makepkg produced no package" >&2; exit 1; }
cp -f "$pkg" "$repo_dir/repo/"

# Pin the exact Boost.Python soname used to compile this package.
for package in boost boost-libs; do
    version=$(pacman -Q "$package" | awk '{print $2}')
    cached=$(find "$repo_dir/cache" -maxdepth 1 -type f \
        -name "${package}-${version}-*.pkg.tar.*" -print -quit)
    [[ -n $cached ]] || { echo "build-calamares: cached package missing for ${package} ${version}" >&2; exit 1; }
    cp -f "$cached" "$repo_dir/repo/"
done

repo-add "$repo_dir/repo/noxis-local.db.tar.gz" "$repo_dir"/repo/*.pkg.tar.zst >/dev/null
echo "build-calamares: built $(basename -- "$pkg") against $(pacman -Q boost-libs)"

if [[ -n $profile_copy ]]; then
    mkdir -p "$profile_copy"
    cp -a "${source_profile}/." "$profile_copy/"
    {
        printf '[noxis-local]\nSigLevel = Optional TrustAll\nServer = file://%s/repo\n\n' "$repo_dir"
        cat "${source_profile}/pacman.conf"
    } >"${profile_copy}/pacman.conf"
    echo "build-calamares: profile repo copy prepared at $profile_copy"
fi
