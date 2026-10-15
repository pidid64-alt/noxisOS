# NoxisOS Cachy Edition

> A bootable Linux distribution built on top of **CachyOS** (which is built on
> **Arch Linux**), shipped with a **Hyprland/Wayland** desktop, an offline-capable
> graphical installer, and the NoxisOS branding from this repository.

This directory is a complete [archiso](https://wiki.archlinux.org/title/Archiso)
profile plus the build tooling around it. `make iso` produces a hybrid ISO that
boots on BIOS and UEFI, runs a live Hyprland session, and installs itself to a
disk with Calamares — **without any network connection**.

```
        _ __   _____   __
       | '_ \ / _ \ \ / /     NoxisOS Cachy Edition
       | | | | (_) \ V /      Arch/CachyOS rolling · linux-cachyos · Hyprland
       |_| |_|\___/ \_/
```

---

## Кратко по-русски

* Это полноценный дистрибутив на базе CachyOS: свой ISO-профиль, свой рабочий
  стол (Hyprland), свой установщик и брендинг.
* Сборка — одной командой на Arch/CachyOS-хосте: `./build.sh` (или `make iso`).
  В облаке — через GitHub Actions: **Actions → NoxisOS ISO → Run workflow**.
* Готовый ISO (~3.4 ГБ) лежит на странице
  [Releases](https://github.com/pidid64-alt/noxisOS/releases) — плюс он же
  доступен артефактом сборки. Образ разбит на части по ~1,9 ГБ (ограничение
  GitHub — 2 ГБ на файл), поэтому **качать нужно все части**.
* Склейка и проверка — одной командой: `./scripts/assemble-iso.sh`
  (Linux/macOS) или `scripts\assemble-iso.ps1` (Windows PowerShell). Скрипт сам
  проверяет хеши и отказывается делать обрезанный образ — именно он даёт
  «no bootable device» при записи на флешку.
* Запись на флешку: `sudo dd if=noxis.iso of=/dev/sdX bs=4M status=progress oflag=sync`
  (на Windows — Rufus в режиме *DD image mode*).
* В live-сессии: `Super+Return` — терминал, `Super+D` — меню, `noxis-install` —
  установщик, `noxis-welcome` — подсказки.

---

## ✨ What's inside

| Piece | What it is |
|-------|------------|
| Base | CachyOS repositories (`[cachyos]`, optional `v3`/`v4`/`znver4` tiers) on top of Arch `core`/`extra`/`multilib` |
| Kernel | `linux-cachyos` (BORE scheduler, PGO-tuned) **and** `linux-cachyos-lts` as a fallback, both selectable in the boot menu |
| Desktop | Hyprland + waybar + wofi + mako + hyprlock/hypridle/hyprpaper, themed violet-on-black |
| Installer | Calamares (offline: it unpacks the live rootfs onto the target disk), branded `noxis` |
| Extras | `chwd` hardware detection, CachyOS mirror ranking, zram swap, `paru` (AUR), dGPU/VM guest packages |
| Boot | syslinux (BIOS) + systemd-boot (UEFI); live entries for the standard kernel, the LTS kernel and a `nomodeset` safe mode |

The live medium auto-logs in as `liveuser` on tty1 and starts Hyprland directly.
The installer removes the live-only parts (autologin, passwordless sudo, archiso
initramfs hooks, the live user itself) from the target system — see
[`docs/architecture.md`](docs/architecture.md).

---

## 🚀 Quick start

### Build locally

On any Arch-based host (CachyOS included):

```bash
cd noxis-linux
sudo pacman -S --needed archiso git squashfs-tools dosfstools mtools \
                        libisoburn syslinux grub edk2-shell zstd
./build.sh                 # -> out/noxis-<date>-x86_64.iso
./build.sh --tier v3       # use the x86-64-v3 optimized packages as the base
```

`build.sh` refuses to run as root on a normal machine (mkarchiso builds
unprivileged); CI containers may set `NOXIS_ALLOW_ROOT=1`.

### Build in GitHub Actions

Push a `v*` tag (or open a PR / push to a branch — then the cheap **Verify**
job alone runs) and the workflow does the rest:

| Job | What it does |
|-----|--------------|
| **Verify package list** | resolves every package against the CachyOS+Arch mirrors, runs `scripts/self-check.py` and shellcheck |
| **Build ISO (base)** | `build.sh --clean --no-verify` in an official Arch container, splits the image into <2 GiB parts |
| **Publish the release** | attaches the parts, `SHA256SUMS` and `noxis-*.iso.sha256` to the release for that tag |

```bash
git tag -a v2026.11-alpha -m "NoxisOS Cachy Edition 2026.11 alpha" && git push origin v2026.11-alpha
```

`workflow_dispatch` (Actions → *NoxisOS ISO* → **Run workflow**, optionally
picking a repository tier) is enabled as well and produces the same artifact in
~20–25 minutes without touching a release.

### Download, join, verify

Download **every** file of the release into one folder — a partial join is the
main cause of "no bootable device". Then use the helper, which checks each part
against `SHA256SUMS`, joins them in order and verifies the image against
`noxis-*.iso.sha256` (it stops with an explanation instead of writing a
truncated file):

```bash
noxis-linux/scripts/assemble-iso.sh                 # Linux / macOS
noxis-linux/scripts/assemble-iso.sh --parts ~/Downloads
```

```powershell
# Windows PowerShell
.\noxis-linux\scripts\assemble-iso.ps1 -Parts "$env:USERPROFILE\Downloads" -Out noxis.iso
```

By hand it is the same thing, but nothing stops you from joining one part:

```bash
cat noxis-*.iso.part* > noxis.iso        # Linux / macOS
copy /b noxis-<date>-x86_64.iso.part01+noxis-<date>-x86_64.iso.part02 noxis.iso   # Windows cmd
sha256sum -c SHA256SUMS                  # the parts, as downloaded
sha256sum noxis.iso                      # must equal the hash in noxis-*.iso.sha256
```

| File | Size (bytes) | |
|------|--------------|---|
| `noxis-2026.10.14-x86_64.iso.part01` | 1 992 294 400 | 1,86 GiB |
| `noxis-2026.10.14-x86_64.iso.part02` | 1 656 930 304 | 1,54 GiB |
| joined `noxis-2026.10.14-x86_64.iso` | **3 649 224 704** | **3,40 GiB — this is the number to check** |

### Write the stick

Write the image **raw to the whole device** — not to a partition and not as a
file copy:

```bash
sudo dd if=noxis.iso of=/dev/sdX bs=4M status=progress oflag=sync
sudo cmp -n $(stat -c%s noxis.iso) noxis.iso /dev/sdX && echo 'stick OK'
```

On Windows use [Rufus](https://rufus.ie) in *DD image mode* (or balenaEtcher);
"ISO image mode" is a risk if you also want to check the size afterwards.

### Try it in a VM

The image is a real hybrid: BIOS boots through El Torito + `isohybrid` MBR,
UEFI through El Torito + an embedded EFI system partition, so it works both as
a **CD/DVD image** and as a **disk image** — pick whichever is easier.

```bash
# QEMU: as a CD
qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 -cdrom noxis.iso -boot d \
  -device virtio-vga-gl -display gtk,gl=on
# QEMU: as a disk (BIOS and UEFI with -bios .../OVMF_CODE.fd)
qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 -bios /usr/share/edk2/x64/OVMF.4m.fd \
  -device virtio-vga-gl -display gtk,gl=on \
  -drive file=noxis.iso,format=raw,if=virtio
```

| Hypervisor | What to do |
|------------|------------|
| **VirtualBox** | *Settings → Storage → Controller: IDE → Add Optical Drive → Choose a disk file* → `noxis.iso`. Enable *System → Motherboard → Enable EFI* if you want to test UEFI, and *Display → Graphics Controller: VMSVGA* + *Enable 3D Acceleration* for Hyprland |
| **VMware Workstation/Fusion** | *VM settings → CD/DVD (SATA) → Use ISO image* → the file; *Options → Advanced → Firmware type: UEFI* for the UEFI path |
| **Hyper-V** | Generation 2 (UEFI) works: *Settings → DVD Drive → Insert Disk* → `noxis.iso`. **Turn Secure Boot off** (*Security → Secure Boot → Disable*) — the shipped bootloader is unsigned, and this is the usual "no bootable device" in Hyper-V |
| **virt-manager / Proxmox** | Add a CDROM device with the ISO, or import the ISO as a disk (`raw`); set the boot order to it |

Checks when the firmware insists on "no bootable device":

1. The file that you attached must be the **joined** image: `3 649 224 704` bytes for the 2026.10 release. Two `dd` calls onto the same output file silently overwrite each other — the second one starts at offset 0 again — which is exactly how a 3.4 GiB ISO turns into a 1.6 GiB one that boots nothing. Use `cat part01 part02 > noxis.iso` (or the `assemble-iso` helpers), never `dd of=<same file>` twice, and add `oflag=append conv=notrunc` if you really are appending to a device.
2. Attach it as **CD/DVD**, or as a disk with the right firmware. A UEFI machine booting nothing usually means the image went to an IDE port whose controller is not bootable.
3. Try the other firmware mode (BIOS ↔ UEFI) and disable Secure Boot.

Hyprland needs 3D acceleration: `virtio-vga-gl`/`-display gl=on` in QEMU, VMSVGA + 3D in VirtualBox, *Accelerate 3D graphics* in VMware. Without it, boot the `safe graphics` entry and use the plain TTY.

---

## 📁 Layout

```
noxis-linux/
├── build.sh                     # the one command you need
├── Makefile                     # make iso / deps / verify / assets / tier-v3
├── assets/                      # master artwork (logo + wallpaper)
├── configs/noxis/               # the archiso profile
│   ├── profiledef.sh            #   identity, boot modes, image options
│   ├── packages.x86_64          #   everything that goes into the ISO
│   ├── pacman.conf              #   build-host repo set (CachyOS + Arch)
│   ├── bootstrap_packages.x86_64
│   ├── syslinux/ efiboot/       #   BIOS + UEFI boot menus
│   └── airootfs/                #   the live/installed system itself
│       ├── etc/                 #     os-release, pacman.conf, calamares/, sddm, ...
│       ├── etc/skel/            #     the Hyprland dotfiles users get
│       ├── usr/local/bin/       #     noxis-welcome, noxis-install, noxis-repo-tier, ...
│       └── usr/share/noxis/     #     logo, ASCII art, backgrounds
├── docs/                        # build / architecture / customization / troubleshooting
└── scripts/                     # package pre-flight, repo-tier switcher, artwork generator
```

---

## 🛠 Common tasks

| Task | Command |
|------|---------|
| Verify the package list against the live repos | `./scripts/check-packages.sh` |
| Install the build dependencies | `./build.sh --deps` |
| Switch to the v3/v4/znver4 package tier | `./build.sh --tier v3` (or `./scripts/switch-repo-tier.sh v3`) |
| Regenerate the branding PNGs | `python3 scripts/make-branding-assets.py` (needs Pillow) |
| Change packages | edit `configs/noxis/packages.x86_64` |
| Change the desktop defaults | edit `configs/noxis/airootfs/etc/skel/` |

More: [`docs/build.md`](docs/build.md) · [`docs/architecture.md`](docs/architecture.md) ·
[`docs/customization.md`](docs/customization.md) · [`docs/troubleshooting.md`](docs/troubleshooting.md)

---

## ⚠️ Good to know

* **Rolling release.** The ISO is a snapshot; after installing, run
  `sudo pacman -Syu`. CachyOS repos move fast, so an older ISO may need a large
  first update.
* **CPU tiers.** An ISO built with `--tier v3/v4/znver4` only boots on CPUs that
  support that level. The default (`base`) runs on any x86-64 CPU.
* **Installed system == live system.** `unpackfs` copies the live rootfs, so
  whatever is in `packages.x86_64` and `airootfs/` is exactly what the user ends
  up with (minus the live-only bits removed by `noxis-prepare`).
* **Not affiliated with CachyOS or Arch Linux.** This project reuses their
  repositories and packages under their respective licences; the NoxisOS layer
  is the code in this directory.
