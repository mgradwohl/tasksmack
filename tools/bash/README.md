# Bash Configuration Files

Portable bash configurations for consistent shell environments across machines.

## Files

- **`.bashrc`** - Full-featured bash configuration for normal terminal use
  - Oh-my-posh theme (jandedobbeleer)
  - FZF history search (Ctrl+R)
  - WSL-specific helpers
  - Clang/Ninja defaults
  - Color aliases and completion

- **`.bashrc.copilot`** - Minimal bash configuration for VS Code integrated terminals
  - No Oh-my-posh (agents get confused by ANSI codes)
  - Simple PS1 prompt
  - Deterministic output
  - VS Code shell integration support

- **`.bash_profile`** - Login shell wrapper that sources `.bashrc`

## Installation

### Linux / WSL

```bash
# Copy configs to home directory
cp tools/bash/.bashrc ~/.bashrc
cp tools/bash/.bashrc.copilot ~/.bashrc.copilot
cp tools/bash/.bash_profile ~/.bash_profile

# Reload current shell
source ~/.bashrc
```

### VS Code Workspace Settings

The workspace is already configured to use `~/.bashrc.copilot` for integrated terminals. On a new machine, ensure your `.vscode/settings.json` includes:

```jsonc
{
    "terminal.integrated.profiles.linux": {
        "bash-copilot": {
            "path": "/bin/bash",
            "args": ["--rcfile", "/home/mattg/.bashrc.copilot"]
        }
    },
    "terminal.integrated.defaultProfile.linux": "bash-copilot",
    "github.copilot.chat.terminal.profile": "bash-copilot",
    "terminal.integrated.automationProfile.linux": {
        "path": "/bin/bash",
        "args": ["--rcfile", "/home/mattg/.bashrc.copilot"]
    }
}
```

## Usage

### Normal Terminals (External)

Use your regular shell with full oh-my-posh theme:
```bash
# Just open any terminal - ~/.bashrc loads automatically
```

### VS Code Integrated Terminal

VS Code automatically loads the minimal `~/.bashrc.copilot` profile:
- Clean output for agents (no ANSI decorations)
- Deterministic environment
- Works with GitHub Copilot workspace tools

## Customization

### Update Both Configs

When making changes:
1. Edit `~/.bashrc` or `~/.bashrc.copilot` in your home directory
2. Test your changes
3. Copy back to repo for version control:
   ```bash
   cp ~/.bashrc tools/bash/.bashrc
   cp ~/.bashrc.copilot tools/bash/.bashrc.copilot
   ```

### Adding Aliases

For **normal terminals** (oh-my-posh, colors, etc.), add to `~/.bashrc`.

For **VS Code terminals**, keep `~/.bashrc.copilot` minimal - only essential exports and aliases that don't produce fancy output.

## Dependencies

### Required
- `bash` (4.0+)

### Optional (for full `.bashrc`)
- `oh-my-posh` - Theme engine
- `fzf` - Fuzzy history search
- `clang-22` - Preferred compiler
- `dircolors` - LS_COLORS support

### Installing oh-my-posh

```bash
# Linux
curl -s https://ohmyposh.dev/install.sh | bash -s

# Or via package manager
brew install oh-my-posh  # macOS/Linux
winget install JanDeDobbeleer.OhMyPosh  # Windows
```

## Troubleshooting

### VS Code terminals show decorations

- Verify workspace settings point to `bash-copilot` profile
- Check that `~/.bashrc.copilot` exists and is minimal
- Restart VS Code terminals

### oh-my-posh not working in normal terminals

- Verify `oh-my-posh` is in PATH: `which oh-my-posh`
- Check theme exists: `oh-my-posh config get --list | grep jandedobbeleer`
- Test manually: `eval "$(oh-my-posh init bash --config jandedobbeleer)"`

### FZF Ctrl+R not working

- Install fzf: `sudo apt install fzf` (Linux) or `brew install fzf` (macOS)
- Verify bind: `bind -P | grep fzf`
