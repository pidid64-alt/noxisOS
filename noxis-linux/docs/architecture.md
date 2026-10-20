# How the NoxisOS image is put together

Two systems live in this profile: the **live medium** (what the ISO boots into)
and the **installed system** (what ends up on the user's disk). Because the
installer copies the live rootfs, they are almost the same thing — the
differences are deliberate and are listed below.

```
               mkarchiso
                   │
    bootstrap_packages.x86_64 ──► base chroot
                   │
      packages.x86_64 ──► full live system (kernel, Hyprland, Calamares, …)
                   │
        airootfs/ ──► overrides, dotfiles, branding, systemd units
                   │
              squashfs (arch/x86_64/airootfs.sfs)
                   │
      syslinux (BIOS) + systemd-boot (UEFI) ──► .iso
                   │
        boot ──► live Hyprland session (autologin liveuser)
                   │
        Calamares ──► unpackfs ──► target disk ──► noxis-prepare/finalize
```

## 1. Build-time pieces

| File | Role |
|------|------|
| `profiledef.sh` | ISO name/label/version, boot modes (`bios.syslinux` + `uefi.systemd-boot`), squashfs options, file modes |
| `pacman.conf` (profile root) | repositories used **while building**: `[cachyos]` + Arch |
| `bootstrap_packages.x86_64` | minimal chroot: `base`, `arch-install-scripts`, `cachyos-keyring` |
| `packages.x86_64` | everything the user gets: kernel(s), Hyprland stack, apps, installer, VM guests |
| `syslinux/`, `efiboot/` | boot menu entries (standard kernel / LTS / safe graphics) |

## 2. Live medium specifics (`airootfs`)

* **Accounts.** There is no static `/etc/passwd` override. `usr/lib/sysusers.d/noxis-live.conf`
  creates `liveuser` (uid 1000, memberships in `wheel`, `audio`, `video`, …) at
  every boot, and `usr/lib/tmpfiles.d/noxis-live.conf` owns `/home/liveuser`.
  This keeps the ISO's account database consistent with the packages that
  create other system users (an override that shadows package-created users
  makes their post-install scripts fail).
* **Autologin.** `getty@tty1.service.d/autologin.conf` logs `liveuser` in on
  tty1 and `~/.bash_profile` execs `Hyprland` when the shell is on tty1 — that
  is how the desktop appears without a display manager. No SDDM on the live
  medium.
* **First boot.** `noxis-live-setup.service` → `/usr/local/bin/noxis-live-setup`
  seeds `/home/liveuser` from `/etc/skel`, generates the locales, and refreshes
  the desktop/icon caches once per boot.
* **Keyring.** `/etc/pacman.d/gnupg` is a tmpfs (`etc-pacman.d-gnupg.mount`) and
  `pacman-init.service` repopulates the Arch *and* CachyOS keys on every boot.
  The tmpfs also hides the keyring that `pacstrap` copied from the build host.
* **Passwordless privileges.** `sudoers.d/10-liveuser` and
  `polkit-1/rules.d/49-nopasswd_global.rules` grant `wheel` everything without a
  password, so the installer, gparted and NetworkManager just work.
* **Installer access.** The menu and Waybar button call `noxis-install`, which
  launches Calamares through XWayland with a narrowly restored `DISPLAY` and
  `XAUTHORITY`. The package is rebuilt from CachyOS source for each ISO against
  the same current Boost libraries as the image; the prebuilt rolling package
  lagged a Boost SONAME bump and could not start.
* **Repositories.** `etc/pacman.conf` carries `#<noxis-tier-v3|v4|znver4>`
  blocks; `noxis-repo-tier` toggles them and reinstalls packages on request.
* **Branding files owned by packages.** `/usr/lib/os-release` (package
  `filesystem`) and `/etc/lsb-release` (package `lsb-release`, pulled in as a
  dependency) may not be shipped in `airootfs` — archiso copies it before
  installing packages and pacman aborts with `exists in filesystem`. The branded
  `/etc/os-release` *is* ours (`filesystem` keeps backup files intact), and the
  branded `/etc/lsb-release` is installed from `/usr/share/noxis/lsb-release` by
  `noxis-live-setup` (live) and `noxis-prepare` (installed system).
* **Logs in RAM.** `journald` is volatile on the live medium, persistent after
  install.

## 3. The install itself

Calamares runs the `noxis` branding and this sequence (see `etc/calamares/settings.conf`).

> **Why every config is called `noxis-<module>.conf`:** archiso copies `airootfs/`
> into the build root *before* it installs packages, and the locally rebuilt `noxis-calamares`
> package owns the stock `/etc/calamares/modules/<module>.conf` files. Shipping a
> file with the same name aborts the build with `exists in filesystem`. The
> overrides are therefore bound to their modules through `instances:` in
> `settings.conf` (exactly what CachyOS does with `welcome_online.conf`).
> `/etc/calamares/settings.conf` and `branding/noxis/` are *not* owned by any
> package, so they keep their canonical paths.

| Phase | Modules | Notes |
|-------|---------|-------|
| show | welcome → locale → keyboard → partition → users → summary | no netinstall page: the ISO is self-contained |
| exec | partition, mount, **unpackfs**, machineid, fstab, locale, keyboard, localecfg, removeuser, users, networkcfg, displaymanager, hwclock | `unpackfs` unsquashes `airootfs.sfs` onto the target |
| exec | `shellprocess@prepare` | `/etc/calamares/scripts/noxis-prepare` (see below) |
| exec | initcpiocfg, initcpio, bootloader, grubcfg, services-systemd | mkinitcpio rebuilds a normal initramfs for both kernels; GRUB is installed for BIOS or UEFI |
| exec | `shellprocess@finalize`, preservefiles, umount | `/etc/calamares/scripts/noxis-finalize` |
| show | finished | reboot |

### `noxis-prepare` (inside the target)

Removes everything that only makes sense live:

```
getty@tty1 autologin        noxis-live-setup.service + script
pacman-init.service         etc-pacman.d-gnupg.mount
sysusers.d / tmpfiles.d     live-only entries
sudoers.d/10-liveuser       polkit 49-nopasswd_global.rules
mkinitcpio.conf.d/archiso.conf   (archiso hooks must not reach the target)
journald volatile config    /home/liveuser, /.Trash-1000, skel/.bash_profile
/usr/local/bin/noxis-install + its .desktop entry
```

It also writes the installed system's `/etc/issue`, `/etc/motd` and
`/etc/noxis-installed`.

### `noxis-finalize` (last job)

* initialises the target's pacman keyring (Arch + CachyOS) if it is empty,
* enables `sddm`, `NetworkManager`, `bluetooth`, `cups`, `fstrim.timer`,
  `systemd-timesyncd`, `power-profiles-daemon` and disables `dhcpcd`,
* runs `noxis-repo-tier --auto --no-reinstall` so the target's `pacman.conf`
  matches the CPU (the first `pacman -Syu` then pulls the optimized builds),
* writes the welcome `/etc/motd` and deletes `/etc/calamares`.

## 4. Boot menus

| Entry | Kernel | Extra |
|-------|--------|-------|
| NoxisOS Cachy Edition | `linux-cachyos` | – |
| …LTS kernel | `linux-cachyos-lts` | fallback for new hardware |
| …safe graphics | `linux-cachyos` | `nomodeset` for VMs/virtual GPUs |

syslinux is used for BIOS (`boot/syslinux/`, vesamenu with the NoxisOS splash),
systemd-boot for UEFI (`/loader/entries/*.conf`). Both reference
`%INSTALL_DIR%`/`%ARCHISO_UUID%` placeholders that mkarchiso substitutes.

## 5. What is *not* here

* No custom pacman repository (yet) — the NoxisOS layer is configuration and
  artwork, not rebuilt packages. If you later publish a `[noxis]` repo, add it
  to both `pacman.conf` files and to the Calamares keyring step.
* No netboot/PXE, no ARM builds, no Secure Boot signing (`-c`/`-g` options of
  mkarchiso exist but are unused).
* No user-mode components from the bare-metal `noxis` kernel in this repository
  — that is a separate, unrelated learning OS; this directory is the distro.

## Two things mkarchiso does not tell you

* **Modes are not copied.** `airootfs` is copied with `cp -af --no-preserve=ownership,mode`,
  so every executable bit has to be re-declared in `file_permissions` — otherwise
  the file ships as `0644` and `pkexec`/Calamares fail with "Permission denied".
  `scripts/self-check.py` fails when a shebang file is missing from the array.
* **Package-owned paths win.** Anything a package already installs
  (`/usr/lib/os-release`, `/etc/lsb-release`, `/etc/calamares/modules/*.conf`)
  must not be shipped from `airootfs`: pacstrap aborts with
  `exists in filesystem`. The branded `lsb-release` therefore lives in
  `/usr/share/noxis/` and is copied into place by `noxis-live-setup` /
  Calamares' `noxis-prepare`, and Calamares overrides are named
  `noxis-<module>.conf` and bound through `instances:`.
