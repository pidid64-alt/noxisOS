# Building the NoxisOS ISO

Everything in this document assumes you are on an Arch-based host (Arch,
CachyOS, EndeavourOS, …) or inside the `archlinux:latest` container used by CI.

## 1. Requirements

| Kind | What |
|------|------|
| Packages | `archiso git squashfs-tools dosfstools mtools libisoburn syslinux grub edk2-shell zstd` |
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

1. `scripts/check-packages.sh` resolves every entry of `packages.x86_64` against
   the repositories in `configs/noxis/pacman.conf` (fast failure beats a 40
   minute build that dies at 90%).
2. `mkarchiso -v -w work -o out configs/noxis` boots the profile:
   it installs `bootstrap_packages.x86_64`, then `packages.x86_64` into
   `work/x86_64/airootfs`, copies the profile's `airootfs/` over the result,
   runs the mkinitcpio preset, builds the squashfs, prepares the syslinux and
   systemd-boot tree and finally calls `xorriso`.
3. The result is `out/noxis-<date>-x86_64.iso` (hybrid: BIOS + UEFI, bootable
   from USB via `dd`).

Typical timings on 4 vCPU CI: 25–45 minutes, ~3 GiB ISO, ~11 GiB peak disk.

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

`.github/workflows/noxis-iso.yml` has two jobs:

| Job | When | What |
|-----|------|------|
| `verify` | every push/PR touching `noxis-linux/**` | installs prerequisites, trusts the CachyOS key, resolves the package list, parses every shell script and Calamares YAML config |
| `build` | `workflow_dispatch`, `v*` tags or a published release | builds the ISO in an `archlinux:latest` container and uploads it |

GitHub refuses single files above 2 GiB, so the ISO is split:

```bash
cat noxis-2026.10.14-x86_64.iso.part* > noxis.iso
sha256sum -c noxis-2026.10.14-x86_64.iso.sha256
```

Tag builds (`v1.0` etc.) additionally attach the parts to the GitHub release.

## 5. Rebuilding after an upstream change

* **A package was renamed/removed upstream** → `verify` fails and prints the
  missing names; fix `packages.x86_64` and push.
* **pacman wants to replace a package** → add it to the list explicitly or drop
  the removed one; `pacman -S --print` in `check-packages.sh` shows conflicts.
* **ISO gets too big** → drop `linux-cachyos-lts`, `firefox-i18n-*`, or switch
  the squashfs compression in `profiledef.sh` (`zstd -19` → `xz -Xbcj x86`).

See [troubleshooting.md](troubleshooting.md) for the failure modes seen in
practice.
