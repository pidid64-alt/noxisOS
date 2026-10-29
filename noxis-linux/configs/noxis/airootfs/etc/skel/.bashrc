# ===========================================================================
#  noxis-linux — interactive bash setup
# ===========================================================================
[[ $- != *i* ]] && return

# history
HISTFILE=~/.bash_history
HISTSIZE=10000
HISTFILESIZE=20000
shopt -s histappend checkwinsize cmdhist lithist

# colour support for ls/grep
alias ls='ls --color=auto --group-directories-first'
alias ll='ls -alh --color=auto --group-directories-first'
alias la='ls -Ah --color=auto --group-directories-first'
alias lt='ls -alhF --color=auto --group-directories-first --sort=time'
alias grep='grep --color=auto'
alias fgrep='fgrep --color=auto'
alias egrep='egrep --color=auto'
alias diff='diff --color=auto'
alias ip='ip --color=auto'
alias ..='cd ..'
alias ...='cd ../..'
alias ....='cd ../../..'

# pacman / paru helpers
alias update='sudo pacman -Syu'
alias install='sudo pacman -S'
alias remove='sudo pacman -Rns'
alias search='pacman -Ss'
alias info='pacman -Si'
alias orphan='sudo pacman -Rns $(pacman -Qtdq)'
alias cleanup='sudo paccache -rk2'
alias mirror='sudo cachyos-rate-mirrors'
alias aur='paru -S'
alias aursearch='paru -Ss'

# noxis helpers
alias ff='fastfetch'
alias fetch='fastfetch --config /etc/fastfetch/config.jsonc'
alias please='sudo'
alias welcome='noxis-welcome'
alias install-os='noxis-install'
alias hardware='noxis-hardware'
alias hypr-conf='$EDITOR ~/.config/hypr/hyprland.conf'
alias waybar-conf='$EDITOR ~/.config/waybar/config.jsonc'
alias reload-hypr='hyprctl reload'
alias tasks='systemctl --user list-units --type=service'
alias logs='journalctl -xe'

# prompt: user@host in path, git-aware branches, noxis colours
__noxis_git_branch() {
    command -v git >/dev/null || return
    local b
    b=$(git rev-parse --abbrev-ref HEAD 2>/dev/null) || return
    printf ' \[\e[38;2;34;211;238m\](%s)\[\e[0m\]' "$b"
}
PS1='\[\e[38;2;139;92;246m\]\u\[\e[0m\]@\[\e[38;2;34;211;238m\]\h\[\e[0m\] \[\e[38;2;161;161;170m\]\w\[\e[0m\]$(__noxis_git_branch) \[\e[38;2;139;92;246m\]❯\[\e[0m\] '

# show the banner once per shell (press any key skips it)
# greet on interactive shells: the first terminal of a session gets the
# full fastfetch + hints, later ones just a one-line reminder
if [[ -t 1 && -z ${NOXIS_QUIET:-} && -x /usr/local/bin/noxis-welcome ]]; then
    if [[ -e /tmp/.noxis-greeted ]]; then
        printf '[38;2;139;92;246mnoxis[0m: Super+Z for fetch, Super+W for hints
'
    else
        touch /tmp/.noxis-greeted
        /usr/local/bin/noxis-welcome
    fi
fi
