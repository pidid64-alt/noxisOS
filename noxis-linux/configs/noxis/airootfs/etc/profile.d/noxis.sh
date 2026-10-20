# NoxisOS shell defaults
export EDITOR="${EDITOR:-nvim}"
export VISUAL="${VISUAL:-$EDITOR}"
export PAGER="${PAGER:-less}"
export MANPAGER="${MANPAGER:-less -R}"
export MOZ_ENABLE_WAYLAND=1
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland;xcb}"
export ELECTRON_OZONE_PLATFORM_HINT="${ELECTRON_OZONE_PLATFORM_HINT:-auto}"
