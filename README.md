# eKilo — 2.1.0

**A modern, fast, and delightful terminal text editor.**

eKilo is a heavily enhanced fork of [antirez/kilo](https://github.com/antirez/kilo), evolved through [antonio-foti/ekilo](https://github.com/antonio-foti/ekilo). It keeps the original spirit — **single-file, zero dependencies, extremely fast**.

---

## ✨ Key Features

- **Multi-tab interface** (`Ctrl-T`, `Ctrl-N`/`Ctrl-P`, `Ctrl-W`)
- **Smart undo/redo** — merges consecutive typing, supports bulk operations via snapshots
- **Full regex search** (`Ctrl-E`) + **regex replace-all** with backreferences (`\0`–`\9`) (`Ctrl-R`)
- **Persistent sessions** — reopens your files, cursor positions, and even unsaved changes
- **Syntax highlighting** (C/C++, Python — easily extensible)
- **Line numbers** (toggle with `Ctrl-\`)
- **Auto-pairing** for brackets, quotes, and backticks
- **Go to line** with relative jumps and column support (`Ctrl-G`)
- **Beautiful built-in help** (`F1` or `Ctrl-/`)
- **Zero dependencies**, ~2600 lines of clean C, heavily optimized build

---

## Keyboard Shortcuts

### Global & Help
| Key               | Action                          |
|-------------------|---------------------------------|
| `F1` or `Ctrl-/`  | Toggle help (scrollable)        |
| `Ctrl-Q`          | Quit (3× if unsaved changes)    |
| `Ctrl-L`          | Refresh screen                  |

### Files & Tabs
| Key               | Action                              |
|-------------------|-------------------------------------|
| `Ctrl-S`          | Save (prompts for name if unnamed)  |
| `Ctrl-O`          | Open file in new tab                |
| `Ctrl-T`          | New empty tab                       |
| `Ctrl-N` / `Ctrl-P` | Next / Previous tab               |
| `Ctrl-W`          | Close tab (2× if modified)          |

### Navigation & Editing
| Key                    | Action                              |
|------------------------|-------------------------------------|
| Arrow keys / `Home` / `End` | Standard movement               |
| `PgUp` / `PgDn`        | Page scrolling                      |
| `Ctrl-G`               | Go to line (`42`, `+10`, `-5`, `42:8`) |
| `Ctrl-\`               | Toggle line numbers                 |
| `Ctrl-Z` / `Ctrl-Y`    | Undo / Redo                         |
| `Ctrl-H`               | Delete current line                 |
| `Tab`                  | Insert tab (respects auto-pairing)  |

### Search & Replace
| Key               | Action                                      |
|-------------------|---------------------------------------------|
| `Ctrl-F`          | Find (plain text)                           |
| `Ctrl-E`          | Find (regex mode)                           |
| `Ctrl-R`          | Replace all (regex, supports `\0`–`\9`)     |

> **Tip:** While in the find prompt, use **arrow keys** to navigate matches. Press `Ctrl-E`/`Ctrl-F` to switch between regex and plain mode on the fly.

---

## Advanced Features

### Smart Undo System
- Consecutive typing is intelligently merged into single undo steps (within ~900ms).
- Bulk operations (regex replace, file loads) are stored as atomic snapshots.
- Per-tab history with proper cursor/scroll restoration.

### Session Persistence
eKilo automatically saves your workspace to `~/.local/state/ekilo/` (respects `$XDG_STATE_HOME`).  
Next time you run `ekilo` with no arguments, it restores **everything** — open tabs, unsaved changes, cursor positions.

### Regex Replace
Supports full backreferences:
- `(\w+)` → `[$1]` becomes `[word]`
- Uses standard extended regex (`regcomp` with `REG_EXTENDED`)

---

## Installation

### From Source (Recommended)

```bash
git clone https://github.com/kargogit/ekilo.git
cd ekilo
make
sudo make install
