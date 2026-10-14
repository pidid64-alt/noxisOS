# ===========================================================================
#  noxis-linux — login shell
# ===========================================================================
[[ -f ~/.bashrc ]] && . ~/.bashrc

# The live medium logs in on tty1 automatically and starts Hyprland.
# (The installer removes this file from the installed system; there SDDM
#  takes care of the session.)
if [[ -z ${WAYLAND_DISPLAY:-} && -z ${DISPLAY:-} && "$(tty 2>/dev/null)" == /dev/tty1 ]]; then
    export XDG_SESSION_TYPE=wayland
    export XDG_CURRENT_DESKTOP=Hyprland
    export XDG_SESSION_DESKTOP=Hyprland
    exec start-hyprland 2>/dev/null || exec Hyprland
fi
