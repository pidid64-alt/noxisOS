#!/usr/bin/env bash
# shellcheck disable=SC2034
# ---------------------------------------------------------------------------
# NoxisOS Cachy Edition — archiso profile definition
# ---------------------------------------------------------------------------
# This profile builds a CachyOS-based live/install ISO:
#   * Arch Linux repositories (core/extra/multilib) + the CachyOS repos
#     ([cachyos], optionally [cachyos-v3] / [cachyos-v4] / [cachyos-znver4])
#   * the linux-cachyos kernel family (BORE/sched-ext flavours)
#   * a Hyprland (Wayland) desktop with NoxisOS dotfiles
#   * the CachyOS Calamares installer, re-branded for NoxisOS
#
# Build it with:  ./noxis-linux/build.sh        (see build.sh --help)
# or directly:    sudo mkarchiso -v -w work -o out noxis-linux/configs/noxis
# ---------------------------------------------------------------------------

iso_name="noxis"
# 32 char max, [A-Z0-9_]
iso_label="NOXIS_$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y%m)"
iso_publisher="NoxisOS <https://github.com/pidid64-alt/noxisOS>"
iso_application="NoxisOS Cachy Edition — Live/Install ISO"
iso_version="$(date --date="@${SOURCE_DATE_EPOCH:-$(date +%s)}" +%Y.%m.%d)"
install_dir="arch"
buildmodes=('iso')
# BIOS via syslinux, UEFI via systemd-boot (same well-tested pair as archiso's
# own releng profile).
bootmodes=('bios.syslinux' 'uefi.systemd-boot')
arch="x86_64"
pacman_conf="pacman.conf"
airootfs_image_type="squashfs"
# zstd keeps CI build times sane and still boots noticeably faster than xz.
airootfs_image_tool_options=('-comp' 'zstd' '-Xcompression-level' '19' '-b' '1M')
bootstrap_tarball_compression=('zstd' '-c' '-T0' '--auto-threads=logical' '--long' '-19')

file_permissions=(
  ["/root"]="0:0:750"
  ["/root/.automated_script.sh"]="0:0:755"
  ["/etc/sudoers.d"]="0:0:750"
  ["/etc/sudoers.d/10-liveuser"]="0:0:440"
  ["/etc/polkit-1/rules.d"]="0:0:750"
  ["/usr/local/bin/noxis-live-setup"]="0:0:755"
  ["/usr/local/bin/noxis-welcome"]="0:0:755"
  ["/usr/local/bin/noxis-install"]="0:0:755"
  ["/usr/local/bin/noxis-hardware"]="0:0:755"
  ["/usr/local/bin/noxis-repo-tier"]="0:0:755"
  # mkarchiso copies airootfs with --no-preserve=mode, so every executable that
  # is not listed here ends up as 0644 in the image (pkexec then fails with
  # "Permission denied"). Keep this list in sync with the shebang files;
  # scripts/self-check.py fails the build when something is missing.
  ["/usr/local/bin/noxis-run-as-root"]="0:0:755"
  ["/etc/calamares/scripts/noxis-prepare"]="0:0:755"
  ["/etc/calamares/scripts/noxis-finalize"]="0:0:755"
)
