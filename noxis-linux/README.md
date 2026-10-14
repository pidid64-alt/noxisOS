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
  доступен артефактом сборки. Образ разбит на части по ~1.9 ГБ (ограничение
  GitHub — 2 ГБ на файл). Склейка и проверка: `cat noxis-*.iso.part* > noxis.iso`,
  затем `sha256sum -c SHA256SUMS` и `sha256sum noxis.iso`.
* Запись на флешку: `sudo dd if=noxis.iso of=/dev/sdX bs=4M status=progress oflag=sync`.
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

Download the parts (from the release page or the run's artifacts), then join,
verify and write the image:

```bash
cat noxis-*.iso.part* > noxis.iso
sha256sum -c SHA256SUMS      # the parts, as downloaded
sha256sum noxis.iso          # must equal the hash inside noxis-*.iso.sha256
sudo dd if=noxis.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

### Try it in a VM

```bash
qemu-system-x86_64 -enable-kvm -m 4096 -smp 4 \
  -device virtio-vga-gl -display gtk,gl=on \
  -drive file=noxis-linux/out/noxis-*-x86_64.iso,format=raw,if=virtio
```

Hyprland needs 3D acceleration: keep `virtio-vga-gl`/`-display gl=on` (or use
`-vga std` with the `safe graphics` boot entry).

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
