export BASHRC_LOADED=1

export PATH="$HOME/.local/bin:$PATH"

# ~/.bashrc (WSL2 + Windows Terminal + VS Code) - mattg

# ------------------------------------------------------------
# Interactive shell guard (keeps non-interactive shells fast/clean)
# ------------------------------------------------------------
case $- in
  *i*) ;;
  *) return ;;
esac

# ------------------------------------------------------------
# History (shared across terminals, no duplicates, useful size)
# ------------------------------------------------------------
HISTCONTROL=ignoredups:erasedups
HISTSIZE=50000
HISTFILESIZE=100000
shopt -s histappend
# append new commands + read new commands from other terminals
PROMPT_COMMAND="history -a; history -c; history -r"
export HISTTIMEFORMAT="%Y-%m-%d %H:%M:%S  "
# ------------------------------------------------------------
# Shell behavior
# ------------------------------------------------------------
shopt -s checkwinsize

# Safer globbing (optional; comment out if you dislike)
shopt -s globstar 2>/dev/null || true

# ------------------------------------------------------------
# Less defaults (nice in WT + VS Code)
# ------------------------------------------------------------
export LESS='-F -R -X -K'
export LESSHISTFILE='-'

# ------------------------------------------------------------
# WSL detection + helpers
# ------------------------------------------------------------
is_wsl() { grep -qi microsoft /proc/version 2>/dev/null; }

if is_wsl; then
  # Open current directory in Windows Explorer
  alias explorer='explorer.exe .'
  # Open files with Windows default app
  alias open='xdg-open'

  # Handy paths
  export WINHOME="/mnt/c/Users/$USER"

  # Git: avoid slow scans on /mnt/* (don’t do heavy builds there anyway)
  export GIT_DISCOVERY_ACROSS_FILESYSTEM=1
fi

# ------------------------------------------------------------
# Color support + basic aliases
# ------------------------------------------------------------
if command -v dircolors >/dev/null 2>&1; then
  test -r ~/.dircolors && eval "$(dircolors -b ~/.dircolors)" || eval "$(dircolors -b)"
fi

alias ls='ls --color=auto'
alias ll='ls -alF'
alias la='ls -A'
alias l='ls -CF'

alias grep='grep --color=auto'
alias egrep='egrep --color=auto'
alias fgrep='fgrep --color=auto'

alias cls='clear'
alias ..='cd ..'
alias ...='cd ../..'
alias ....='cd ../../..'

# Quality-of-life
alias mkdirp='mkdir -p'
alias rmf='rm -f'
alias rmdirf='rm -rf'

# ------------------------------------------------------------
# Dev helpers (safe defaults)
# ------------------------------------------------------------
# Prefer clang++ if you installed it; otherwise system default
if command -v clang-22 >/dev/null 2>&1; then
  alias clang='clang-22'
  alias clang++='clang++-22'
  alias clangd='clangd-22'
fi

# Faster linking when using clang (lld)
# (harmless if clang isn’t used)
export LDFLAGS="${LDFLAGS:+$LDFLAGS }-fuse-ld=lld"

# Ninja defaults (match your WSL CPU tuning; adjust if you want quieter)
export NINJAFLAGS="${NINJAFLAGS:--j12}"

# ------------------------------------------------------------
# Bash completion (only if available)
# ------------------------------------------------------------
if ! shopt -oq posix; then
  if [ -f /usr/share/bash-completion/bash_completion ]; then
    . /usr/share/bash-completion/bash_completion
  elif [ -f /etc/bash_completion ]; then
    . /etc/bash_completion
  fi
fi

__fzf_history__() {
  local cmd
  cmd=$(history | sed 's/^[ ]*[0-9]\+[ ]*//' | tac | fzf --height 40% --reverse --border --prompt="History > ")
  [[ -n "$cmd" ]] && READLINE_LINE="$cmd" && READLINE_POINT=${#cmd}
}
bind -x '"\C-r": __fzf_history__'

# ------------------------------------------------------------
# Oh My Posh (keep it last, and keep it quiet)
# Matches your PowerShell built-in theme: "jandedobbeleer"
# ------------------------------------------------------------
export OMP_NO_UPDATE=1
export OMP_TRANSIENT_PROMPT=0
export OMP_LINE_ERROR=0
export OMP_TOOLTIPS=0

if command -v oh-my-posh >/dev/null 2>&1; then
  # Prefer the built-in theme by name (same as your pwsh profile)
  eval "$(oh-my-posh init bash --config jandedobbeleer)"
fi
