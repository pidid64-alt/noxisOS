# Customizing NoxisOS

Everything user-visible is a plain file in this directory. Nothing is generated
by a build system, so edit → build → boot is the whole workflow (the only
exception is artwork, which is derived from `assets/` by a script).

## Identity

| What | Where |
|------|-------|
| Product name, ISO label, version | `configs/noxis/profiledef.sh` |
| `/etc/os-release`, `/etc/lsb-release`, `/etc/noxis-release` | `airootfs/etc/` |
| Installer name and slideshow | `airootfs/etc/calamares/branding/noxis/branding.desc` |
| Installer colours, buttons, lists | `airootfs/etc/calamares/branding/noxis/stylesheet.qss` |
| Shell banner, tips, ASCII logo | `airootfs/usr/local/bin/noxis-welcome`, `airootfs/usr/share/noxis/ascii-logo.txt` |
| fastfetch layout | `airootfs/etc/fastfetch/config.jsonc` |

## Artwork

Replace the masters and regenerate the derived files:

```bash
# assets/noxis-logo.png       square mark (>=512px)
# assets/noxis-wallpaper.png  wide background (>=1920px)
python3 scripts/make-branding-assets.py      # needs Pillow
```

That writes the desktop wallpaper, the syslinux splash, and the Calamares
`logo/icon/welcome/productWallpaper/slide1..3` images. The generated PNGs are
committed, so a build never needs Pillow.

## Packages

`configs/noxis/packages.x86_64` — one package per line, `#` comments allowed:

```bash
echo kitty >> configs/noxis/packages.x86_64
./scripts/check-packages.sh      # do they all exist?
./build.sh --no-verify
```

Remember the live/installed system is the same image: anything you add is also
installed on the user's disk. Live-only tools belong in the *live-only* section
of the file (bottom) — but note they survive the install unless you delete them
in `noxis-prepare`.

## Desktop defaults

`configs/noxis/airootfs/etc/skel/` is copied into every new user's home:

| Path | What |
|------|------|
| `.config/hypr/hyprland.conf` | keybinds, gaps, blur, window rules, `$wallpaper` |
| `.config/hypr/hypridle.conf`, `hyprlock.conf`, `hyprpaper.conf` | idle/lock/wallpaper |
| `.config/waybar/{config.jsonc,style.css}` | top bar, modules, violet theme |
| `.config/wofi/`, `.config/mako/`, `.config/wlogout/` | launcher, notifications, session menu |
| `.config/{kitty,alacritty,foot}/`, `.config/btop/` | terminal + system monitor themes |
| `.config/gtk-3.0|gtk-4.0|qt5ct|qt6ct` | toolkit theming so apps look native |
| `.bashrc`, `.bash_profile` | aliases, git-aware prompt, autostart of Hyprland on tty1 |

Change the accent colour in one place per file — `#8b5cf6` (violet) and
`#22d3ee` (cyan) are used consistently across Hyprland, waybar, wofi, mako,
kitty/alacritty/foot and Calamares.

## Repositories and CPU tiers

* `pacman.conf` (profile root) — what the **build** uses.
* `airootfs/etc/pacman.conf` — what the **user's system** uses (with the tier
  blocks toggled by `noxis-repo-tier`).
* Default Arch mirrors live in `airootfs/etc/pacman.d/mirrorlist`
  (CachyOS' CDN cache first, then geo mirrors); the CachyOS mirrorlists are
  pinned to `mirror.cachyos.org`.

Build a v3-optimized ISO: `./build.sh --tier v3` (or `make tier-v3`). Add
`znver4` support by extending `scripts/switch-repo-tier.sh` — the tiers map is a
small Python dict at the top.

## Installer behaviour

Everything Calamares does is in `airootfs/etc/calamares/`:

| File | Interesting knobs |
|------|-------------------|
| `settings.conf` | the module sequence and the `instances:` table |
| `modules/noxis-partition.conf` | filesystem list, EFI size, default filesystem, swap choice |
| `modules/noxis-users.conf` | default groups, password rules, hostname template, forbidden names |
| `modules/noxis-mount.conf` | btrfs subvolumes, mount options per filesystem |
| `modules/noxis-bootloader.conf` | systemd-boot vs GRUB, kernel lines, menu timeout |
| `modules/noxis-services-systemd.conf` | which units are enabled in the target |
| `modules/noxis-welcome.conf` | RAM/disk requirements, which checks block the install |
| `scripts/noxis-prepare`, `scripts/noxis-finalize` | the two shell hooks that clean up and finish the target |

Overrides are named `noxis-<module>.conf` on purpose: the `cachyos-calamares`
package owns `/etc/calamares/modules/<module>.conf`, and airootfs files are
copied before packages are installed, so identical names fail the build with
`exists in filesystem`. Each override is bound to its module by an entry in the
`instances:` block of `settings.conf` (see the note in
[architecture.md](architecture.md)). Adding a new one: drop
`modules/noxis-<name>.conf`, add the instance and reference `module@<id>` in the
sequence.

The shell hooks are ordinary bash — the fastest way to add post-install steps
(extra users, firewall defaults, licence files) is to append them to
`noxis-finalize`.

## Boot menus

* `syslinux/*.cfg` — BIOS; `archiso_head.cfg` holds the vesamenu title/colours,
  the entries live in `archiso_sys-linux.cfg`.
* `efiboot/loader/entries/*.conf` + `efiboot/loader/loader.conf` — UEFI.
* `syslinux/splash.png` — regenerate with the artwork script (640×480).

Kernel parameters can be appended in both places; `%KERNEL_PARAMS%` is replaced
by mkarchiso, `%ARCHISO_UUID%`/`%INSTALL_DIR%` are its placeholders.
