# Troubleshooting

## Build fails before it starts

| Symptom | Cause / fix |
|---------|-------------|
| `refusing to run as root` | run `./build.sh` as a normal user; containers/CI set `NOXIS_ALLOW_ROOT=1` |
| `missing: dosfstools mtools …` | `./build.sh --deps`, then rerun |
| `could not sync package databases` | no network, or the Arch mirrorlist is broken. The profile's `pacman.conf` includes `/etc/pacman.d/mirrorlist` from the **host** — regenerate it (`cachyos-rate-mirrors` / `reflector`) |
| `signature from "CachyOS <…>" is unknown trust` | the CachyOS key is missing — see the keyring commands in [build.md](build.md) |

## `check-packages.sh` reports missing packages

The Arch/CachyOS repositories are rolling, so package names come and go.
The script prints the exact names; either drop them from
`configs/noxis/packages.x86_64` or replace them with the current equivalent.
Common ones that have moved: `waybar-hyprland` → `waybar`,
`swaylock-effects-git` (AUR only on some setups), `nwg-look` (extra).

## `error: failed to commit transaction (conflicting files)` / `exists in filesystem`

A file in `airootfs/` has the same path as a file owned by a package, and
archiso copies the profile *before* installing packages. Either rename the
override (the Calamares configs here are `noxis-<module>.conf` bound through
`instances:` for exactly this reason) or let the package own the file and patch
it in one of the `nosix`/shell hooks instead. `/etc/pacman.conf`,
`/etc/skel/*`, `/etc/locale.conf` and friends are fine: they are `backup=()`
files, which pacman keeps as they are.

## mkarchiso warnings are fine, errors are not

* `'edk2-shell' is not in the package list` → it *is* in `packages.x86_64`; if it
  still appears, the package failed to resolve.
* `Cannot change permissions of …` → a path in `profiledef.sh`'s
  `file_permissions` does not exist in your airootfs. Only list files you ship.
* `The 'syslinux' package is missing from the package list!` (fatal) → keep
  `syslinux` in `packages.x86_64`; BIOS boot does not work without it.

## The USB stick does not boot ("no bootable device", "no bootable medium", black screen)

Work through these in order — the first one is by far the most common:

| Check | What to look at |
|-------|-----------------|
| **Is the image complete?** | The release publishes an ISO as several `<2 GiB` parts. Joining only some of them (or letting a download finish halfway) gives a file Windows/Linux tools happily write — and it boots nothing. Expected total for `noxis-2026.10.14-x86_64.iso`: **3 649 224 704 bytes** (`part01` 1 992 294 400 + `part02` 1 656 930 304). Run `noxis-linux/scripts/assemble-iso.sh` (or `assemble-iso.ps1`): it verifies every part against `SHA256SUMS` and the joined image against `noxis-*.iso.sha256` and refuses a truncated result |
| **How was the stick written?** | The ISO must be written **raw to the whole device**: `sudo dd if=noxis.iso of=/dev/sdX bs=4M status=progress oflag=sync`, Rufus in *DD image mode*, or balenaEtcher. Copying the ISO file onto a FAT stick, or writing to `/dev/sdX1` instead of `/dev/sdX`, produces exactly this message |
| **Did the write land?** | `sudo cmp -n $(stat -c%s noxis.iso) noxis.iso /dev/sdX && echo 'stick OK'` — read back and compare, no guessing |
| **Secure Boot** | The shipped `systemd-boot`/syslinux binaries are unsigned, so firmware with Secure Boot on may skip the stick entirely. Disable Secure Boot (BIOS → Security → Secure Boot → *Disabled*) or enroll the binaries |
| **Boot mode** | Both modes are supported, so try the other one: the one-time boot menu (`F12`/`F11`/`Esc`) usually lists the stick twice — as `UEFI: <stick>` and as a plain BIOS entry |
| **VirtualBox/VMware** | These emulate BIOS but often need EFI enabled to boot hybrid ISOs, and Hyprland needs 3D acceleration; QEMU with `-device virtio-vga-gl -display gtk,gl=on` is the supported path |
| **Cheap/old stick** | Some controllers corrupt large writes; format nothing, just rewrite with `dd` and re-check with `cmp`, or try another stick |
| **Filesystem of the stick** | Irrelevant — `dd` overwrites the partition table as well. Do **not** format the stick "to make it bootable" first |

After the medium boots, `archiso` finds everything from `/run/archiso/bootmnt`; if the live system starts but the installer complains about `airootfs.sfs`, the medium was again written as a file copy rather than as a raw image.

## The live session does not appear

| Symptom | Check |
|---------|-------|
| Black screen, no login prompt | press `Ctrl+Alt+F2`; `journalctl -b -u noxis-live-setup getty@tty1` |
| `Hyprland` exits immediately | the GPU/driver cannot do OpenGL in the VM — use `virtio-vga-gl`/`-display gl=on`, or boot the “safe graphics” entry (`nomodeset`) |
| No password prompt after autologin | `~/.bash_profile` was overwritten — it must exec Hyprland when `tty == /dev/tty1` |
| `liveuser` does not exist | `systemd-sysusers` failed: `journalctl -b \| grep -i sysusers`; `noxis-live-setup` re-creates the account as a fallback |
| Desktop appears but no bar | `waybar` needs `fontconfig` caches: `fc-cache -f` in the live session, and check `packages.x86_64` still has the JetBrainsMono/Nerd fonts |

## Installing fails

| Symptom | Cause / fix |
|---------|-------------|
| Calamares does not start | run `pkexec /usr/local/bin/noxis-run-as-root calamares -d` in a terminal and read the trace |
| `unpackfs` fails on `airootfs.sfs` | the ISO was booted from something other than the medium (e.g. copied files). `/run/archiso/bootmnt` must point at the ISO |
| “The installer needs at least X GiB” | `modules/welcome.conf` → `requirements.requiredStorage` |
| Target does not boot (UEFI) | check that `/boot/efi` got the ESP; systemd-boot entries are written by the `bootloader` module — see `/var/log/Calamares.log` on the target |
| No kernel modules after install | `mkinitcpio -P` ran with a broken preset — verify `initcpio.conf` still has `kernel: all` and that both kernels were unpacked |
| Keyring errors on the first `pacman -Syu` | `noxis-finalize` only initialises keys when `trustdb.gpg` is missing; if the copy was incomplete run `sudo pacman-key --init && sudo pacman-key --populate archlinux cachyos` |

## CI

| Symptom | Fix |
|---------|-----|
| `verify` fails on YAML | a Calamares config is invalid; the job prints file + parser error |
| `build` cancelled at the 300-minute timeout | drop `linux-cachyos-lts` or `firefox-i18n-*`, or switch `airootfs_image_tool_options` to `zstd` with a lower level |
| `No space left on device` | the runner has ~14 GiB of free space; `--clean` wipes `work/` but the package cache stays — add `pacman -Scc --noconfirm` between runs if needed |
| Artifact download fails | single files must stay below 2 GiB — keep the `split --bytes=1900M` step |

## Where the logs are

* build: mkarchiso output, plus `work/*.log` and `out/`
* live medium: `journalctl -b`, `/var/log/Xorg.0.log` (not used), `hyprland` logs in `$XDG_RUNTIME_DIR/hypr/`
* installer: `/var/log/Calamares.log` on the live medium and — thanks to
  `preservefiles` — `/var/log/Calamares.log` + `Calamares-install.json` on the
  installed system
* live setup script: `journalctl -b -u noxis-live-setup`
