#!/usr/bin/env python3
"""Structural self-check for the NoxisOS archiso profile.

Catches the mistakes that are cheap to find here and expensive to find after a
40-minute ISO build:

  * Calamares configs that are not valid YAML
  * settings.conf referring to modules/instances without a config file
  * profiledef.sh listing file_permissions for files the profile does not ship
  * configs pointing at files that are not in the image (wallpapers, dotfiles)
  * scripts calling binaries that are missing from packages.x86_64

Usage:  python3 scripts/self-check.py [profile-dir]

Exit status: 0 = clean, 1 = problems found, 2 = could not run.
"""

from __future__ import annotations

import re
import sys
import pathlib

try:
    import yaml
except ImportError:  # pragma: no cover
    sys.exit("PyYAML is required: pacman -S python-yaml  |  pip install pyyaml")

# Calamares modules that work with the defaults shipped by the package, so the
# profile does not need to provide a config for them.
BUILTIN_MODULES = {
    "locale", "keyboard", "summary", "localecfg", "networkcfg", "hwclock",
    "initcpiocfg", "umount", "finished", "welcome", "users", "partition",
    "fstab", "mount", "machineid", "unpackfs", "displaymanager", "bootloader",
    "grubcfg", "initcpio", "removeuser", "preservefiles", "services-systemd",
}

# Paths that only exist in the *built* image (packages create them), not in the
# source tree.
RUNTIME_CREATED = {"/etc/shadow", "/etc/gshadow", "/etc/passwd", "/etc/group"}

# binary -> owning package (for the "is it actually installed" check)
BINARY_PACKAGES = {
    "archinstall": "archinstall", "blueman-manager": "blueman", "brightnessctl": "brightnessctl",
    "btop": "btop", "calamares": "cachyos-calamares", "chwd": "chwd", "cliphist": "cliphist",
    "dolphin": "dolphin", "fastfetch": "fastfetch", "firefox": "firefox", "grim": "grim",
    "hyprctl": "hyprland", "hypridle": "hypridle", "hyprlock": "hyprlock",
    "hyprpaper": "hyprpaper", "kitty": "kitty", "mako": "mako", "nvim": "neovim",
    "nm-connection-editor": "nm-connection-editor", "pamixer": "pamixer",
    "paru": "paru", "pavucontrol": "pavucontrol", "playerctl": "playerctl",
    "slurp": "slurp", "swappy": "swappy", "tmux": "tmux", "waybar": "waybar",
    "wl-copy": "wl-clipboard", "wl-paste": "wl-clipboard", "wlogout": "wlogout",
    "wofi": "wofi", "xhost": "xorg-xhost", "update-desktop-database": "desktop-file-utils",
}
# commands that come with the base system, no package entry expected
IGNORED_COMMANDS = {
    "bash", "sh", "python3", "pacman", "pacman-key", "systemctl", "systemd", "loginctl",
    "useradd", "getent", "locale-gen", "install", "cp", "mv", "rm", "mkdir", "chmod",
    "chown", "find", "sort", "sed", "grep", "awk", "curl", "date", "printf", "echo",
    "cat", "test", "true", "false", "stat", "ln", "touch", "xdg-user-dirs-update",
    "gtk-update-icon-cache", "runuser", "sudo", "pkexec", "dbus-update-activation-environment",
    "wayland-session", "start-hyprland", "Hyprland", "localectl", "hwclock",
}


def fail(problems: list[str], message: str) -> None:
    problems.append(message)


def check_calamares(root: pathlib.Path, problems: list[str]) -> None:
    cal = root / "airootfs" / "etc" / "calamares"
    if not cal.is_dir():
        fail(problems, f"no Calamares configuration at {cal}")
        return

    for path in sorted(cal.rglob("*")):
        if path.is_file() and (path.suffix in {".conf", ".desc"} or path.name == "settings.conf"):
            try:
                list(yaml.safe_load_all(path.read_text()))
            except Exception as exc:  # noqa: BLE001 - report any YAML problem
                fail(problems, f"invalid YAML: {path.relative_to(root.parent)}: {exc}")

    settings_path = cal / "settings.conf"
    if not settings_path.is_file():
        return
    try:
        settings = list(yaml.safe_load_all(settings_path.read_text()))[0]
    except Exception:  # already reported above
        return

    instances = {i["id"]: i["config"] for i in settings.get("instances", [])}
    for stage in settings.get("sequence", []):
        for kind, modules in stage.items():
            for module in modules:
                name, _, instance = module.partition("@")
                if instance:
                    if instance not in instances:
                        fail(problems, f"settings.conf: unknown instance '{module}'")
                    elif not (cal / "modules" / instances[instance]).is_file():
                        fail(problems, f"settings.conf: missing config '{instances[instance]}'")
                elif name not in BUILTIN_MODULES and not (cal / "modules" / f"{name}.conf").is_file():
                    fail(problems, f"settings.conf: no configuration for module '{name}'")

    branding = settings.get("branding")
    if branding and not (cal / "branding" / branding / "branding.desc").is_file():
        fail(problems, f"settings.conf: branding '{branding}' has no branding.desc")


def check_profiledef(profile: pathlib.Path, problems: list[str]) -> None:
    profiledef = profile / "profiledef.sh"
    if not profiledef.is_file():
        fail(problems, "profiledef.sh is missing")
        return
    airootfs = profile / "airootfs"
    text = profiledef.read_text()
    entries = dict(re.findall(r'\["([^"]+)"\]="\d+:\d+:(\d+)"', text))
    for path in entries:
        if path in RUNTIME_CREATED:
            continue
        if not (airootfs / path.lstrip("/")).exists():
            fail(problems, f"profiledef.sh: file_permissions lists '{path}', which the profile does not ship")

    # The other direction, and the one that actually bit us: mkarchiso copies
    # airootfs with 'cp -a --no-preserve=ownership,mode', so the executable bit
    # of a shipped script survives only if file_permissions sets it again.
    for path in airootfs.rglob("*"):
        if not path.is_file():
            continue
        try:
            first = path.read_text(errors="ignore").split("\n", 1)[0]
        except OSError:
            continue
        if not first.startswith("#!"):
            continue
        key = "/" + str(path.relative_to(airootfs))
        mode = entries.get(key)
        if mode is None:
            fail(problems, f"profiledef.sh: '{key}' is a script but has no file_permissions entry "
                           f"(mkarchiso does not preserve the executable bit)")
        elif mode[-1] not in "57":
            fail(problems, f"profiledef.sh: '{key}' is a script but file_permissions gives it mode {mode}")



def check_live_installer_and_display(profile: pathlib.Path, problems: list[str]) -> None:
    airootfs = profile / "airootfs"
    desktop = airootfs / "usr/share/applications/noxis-install.desktop"
    if not desktop.is_file():
        fail(problems, "live installer has no application-menu entry at /usr/share/applications/noxis-install.desktop")
    else:
        content = desktop.read_text(errors="replace")
        if "Exec=/usr/local/bin/noxis-install" not in content or "Terminal=false" not in content:
            fail(problems, "NoxisOS installer desktop entry must launch noxis-install without a terminal")

    hypr = airootfs / "etc/skel/.config/hypr/hyprland.conf"
    if not hypr.is_file() or "monitor = , 1920x1080@60, auto, 1" not in hypr.read_text(errors="replace"):
        fail(problems, "live and installed Hyprland config must request 1920x1080@60")

    installer = airootfs / "usr/local/bin/noxis-install"
    if installer.is_file() and "noxis-run-as-root --x11" not in installer.read_text(errors="replace"):
        fail(problems, "noxis-install must launch the root Calamares GUI through XWayland")

    waybar = airootfs / "etc/skel/.config/waybar/config.jsonc"
    if waybar.is_file():
        content = waybar.read_text(errors="replace")
        if '"on-click": "noxis-install"' not in content or "pkexec /usr/local/bin/noxis-run-as-root" in content:
            fail(problems, "Waybar's installer button must call noxis-install, not bypass its XWayland setup")

    bootloader = airootfs / "etc/calamares/modules/noxis-bootloader.conf"
    if bootloader.is_file() and 'efiBootLoader: "grub"' not in bootloader.read_text(errors="replace"):
        fail(problems, "Calamares must default to GRUB (supports legacy BIOS and UEFI)")

def check_referenced_paths(profile: pathlib.Path, problems: list[str]) -> None:
    airootfs = profile / "airootfs"
    skel = airootfs / "etc" / "skel"
    seen: set[str] = set()
    for path in list(skel.rglob("*")) + list((airootfs / "usr" / "local" / "bin").glob("*")):
        if not path.is_file():
            continue
        text = path.read_text(errors="ignore")
        for target in re.findall(r"/usr/share/(?:backgrounds/noxis|noxis)/[\w.\-]+", text):
            if target in seen:
                continue
            seen.add(target)
            if not (airootfs / target.lstrip("/")).exists():
                fail(problems, f"{path.name} references '{target}', which is not in the image")


def check_binaries(profile: pathlib.Path, problems: list[str]) -> None:
    packages = {
        line.strip()
        for line in (profile / "packages.x86_64").read_text().splitlines()
        if line.strip() and not line.startswith("#")
    }
    airootfs = profile / "airootfs"
    sources = [p for p in (airootfs / "etc" / "skel").rglob("*") if p.is_file()]
    sources += list((airootfs / "usr" / "local" / "bin").iterdir())
    sources += list((airootfs / "etc" / "calamares" / "scripts").iterdir())
    text = "\n".join(p.read_text(errors="ignore") for p in sources if p.is_file())

    for binary, package in BINARY_PACKAGES.items():
        if binary in IGNORED_COMMANDS or package in packages:
            continue
        if re.search(rf"(^|[\s/|&;(]){re.escape(binary)}(\s|$|\")", text, re.MULTILINE):
            fail(problems, f"'{binary}' is used by the profile but '{package}' is not in packages.x86_64")

    required_always = {
        "syslinux": "BIOS boot (bios.syslinux)",
        "grub": "the GRUB option in Calamares and mkarchiso's efiboot tooling",
        "mtools": "mkarchiso creates the EFI FAT image with mmd/mcopy",
        "squashfs-tools": "the live rootfs is a squashfs",
        "cachyos-calamares": "the installer",
        "zram-generator": "enables the zram swap unit",
    }
    for package, why in required_always.items():
        if package not in packages:
            fail(problems, f"'{package}' is not in packages.x86_64 ({why})")


def main(argv: list[str]) -> int:
    script_dir = pathlib.Path(__file__).resolve().parent
    profile = pathlib.Path(argv[1]) if len(argv) > 1 else script_dir.parent / "configs" / "noxis"
    if not profile.is_dir():
        print(f"profile directory not found: {profile}", file=sys.stderr)
        return 2

    problems: list[str] = []
    check_calamares(profile, problems)
    check_profiledef(profile, problems)
    check_live_installer_and_display(profile, problems)
    check_referenced_paths(profile, problems)
    check_binaries(profile, problems)

    if problems:
        print(f"self-check found {len(problems)} problem(s):", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1

    files = sum(1 for p in profile.rglob("*") if p.is_file())
    print(f"self-check: OK ({files} files in {profile})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
