# Building the NoxisOS ISO

Everything in this document assumes you are on an Arch-based host (Arch,
CachyOS, EndeavourOS, …) or inside the `archlinux:latest` container used by CI.

## 1. Requirements

| Kind | What |
|------|------|
| Packages | `archiso git squashfs-tools dosfstools mtools libisoburn syslinux grub edk2-shell zstd base-devel boost boost-libs cmake doxygen extra-cmake-modules ninja pacman-contrib python-jsonschema python-pyaml python-unidecode qt6-tools` |
| Disk | ~12 GiB free in `noxis-linux/` (work directory + ISO + package cache) |
| Network | needed **while building** (packages are downloaded); *not* needed at install time |
| Keys | `archlinux-keyring` plus the CachyOS key `F3B607488DB35A47` |

`./build.sh` checks the first three and offers `--deps` to install the packages.
It will also initialise both keyrings for you the first time:

```bash
sudo pacman-key --init
sudo pacman-key --populate archlinux
sudo pacman-key --recv-keys F3B607488DB35A47 --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign-key F3B607488DB35A47
```

## 2. The build

```bash
cd noxis-linux
./build.sh                  # full ISO into out/
./build.sh --clean          # wipe ./work first (fresh, smaller, slower)
./build.sh --no-verify      # skip the package pre-flight check
./build.sh --tier v3        # build with x86-64-v3 optimized base packages
```

What happens under the hood:

1. `scripts/build-calamares.sh` rebuilds our Calamares package from CachyOS
   source against the current Boost/Python ABI, caches the exact `boost` and
   `boost-libs` packages in a local pacman repo, and prepares a temporary profile
   that prefers this repo. This prevents the known stale prebuilt Calamares
   binary from asking for an absent `libboost_python` SONAME.
2. `scripts/check-packages.sh` resolves every entry of `packages.x86_64` against
   the local repository and the repositories in `configs/noxis/pacman.conf`.
3. `mkarchiso -v -w work -o out <temporary-profile>` boots the profile:
   it installs `bootstrap_packages.x86_64`, then `packages.x86_64` into
   `work/x86_64/airootfs`, copies the profile's `airootfs/` over the result,
   runs the mkinitcpio preset, builds the squashfs, prepares the syslinux and
   systemd-boot tree and finally calls `xorriso`.
4. The result is `out/noxis-<date>-x86_64.iso` (hybrid: BIOS + UEFI, bootable
   from USB via `dd`).

Typical timings on 4 vCPU CI: 35–60 minutes (including the Calamares rebuild), ~3.5 GiB ISO, ~11 GiB peak disk.

## 3. Testing

```bash
# BIOS/legacy boot in QEMU
qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 -vga std \
  -drive file=out/noxis-*-x86_64.iso,format=raw,if=virtio

# UEFI boot (install edk2-ovmf first)
qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 \
  -device virtio-vga-gl -display gtk,gl=on \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/x64/OVMF_CODE.4m.fd \
  -drive file=out/noxis-*-x86_64.iso,format=raw,if=virtio

# USB stick
sudo dd if=out/noxis-*.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

Hyprland requires OpenGL: use `virtio-vga-gl` + `-display gl=on` (or a real GPU
with `-device virtio-vga` and passthrough). On plain `-vga std` pick the
**safe graphics** boot entry, which adds `nomodeset`.

## 4. CI (GitHub Actions)

`.github/workflows/noxis-iso.yml` has three jobs:

| Job | When | What |
|-----|------|------|
| `verify` | every push/PR touching `noxis-linux/**` | installs prerequisites, trusts the CachyOS key, resolves the package list, parses every shell script and Calamares YAML config |
| `build` | `workflow_dispatch`, `v*` tags or a published release | builds the ISO in an `archlinux:latest` container, splits it and uploads the parts as an artifact |
| `release` | `v*` tags only | downloads that artifact on the host runner (where `gh` exists) and attaches the parts, `SHA256SUMS` and `noxis-*.iso.sha256` to the release for the tag |

GitHub refuses single files above 2 GiB, so the ISO is split into `part01`,
`part02`, … `build` asserts that the parts reassemble into the image that was
just built (`sha256` of `cat parts*` equals the ISO hash), so a broken split can
never reach a release.

Joining and verifying on your side — the helper does every check and refuses a
truncated image, which is what produces "no bootable device":

```bash
noxis-linux/scripts/assemble-iso.sh --parts ~/Downloads   # Linux / macOS
```

```powershell
.\noxis-linux\scripts\assemble-iso.ps1 -Parts "$env:USERPROFILE\Downloads"
```

By hand: `cat noxis-*.iso.part* > noxis.iso`, then `sha256sum -c SHA256SUMS`
(parts) and `sha256sum noxis.iso` (whole image, hash from
`noxis-*.iso.sha256` — note that file names the image, so compare the hash value
rather than using `sha256sum -c` on a renamed file).

## 5. Rebuilding after an upstream change

* **A package was renamed/removed upstream** → `verify` fails and prints the
  missing names; fix `packages.x86_64` and push.
* **pacman wants to replace a package** → add it to the list explicitly or drop
  the removed one; `pacman -S --print` in `check-packages.sh` shows conflicts.
* **ISO gets too big** → drop `linux-cachyos-lts`, `firefox-i18n-*`, or switch
  the squashfs compression in `profiledef.sh` (`zstd -19` → `xz -Xbcj x86`).

See [troubleshooting.md](troubleshooting.md) for the failure modes seen in
practice.
