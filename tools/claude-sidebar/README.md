# Sidebar + Claude Code status

This fork replaces Windows Terminal's tab strip with a collapsible sidebar:

- Every pane of every tab is listed, grouped by **git repository**, then by **worktree**
  (main worktree first, linked worktrees after it, with their branch names).
  Panes outside a repository are grouped by directory.
- Panes running **Claude Code** show its state:
  - spinner: working (with the number of running subagents)
  - amber dot: waiting for you (a permission prompt, a question, a plan to review, MCP input)
  - green dot: finished while you were looking elsewhere ("Done — your turn")
  - red dot: the turn ended with an API error
  - stopwatch badge: background tasks still running (shells, subagents, monitors); hover for the list
- Collapse the sidebar to an icon rail with the ☰ button or `Ctrl+Shift+B` (`toggleSidebar` action).
- Drag its right edge to resize it. Click a group to fold it. Right-click a terminal for
  "New tab here", "Copy path" and "Close tab". The `+` on a worktree opens a new tab there.
- Set `"showTabsInSidebar": false` in settings.json to get the normal tab strip back.

## How the status gets there

Windows Terminal gives each pane a unique `WT_SESSION` id, which Claude Code and its hooks
inherit. `wt-claude-status.js` is registered as a Claude Code hook (in `~/.claude/settings.json`) and writes
`~/.claude/terminal-status/<WT_SESSION>.json` on each relevant event. The terminal polls these files for
the panes it shows (`src/cascadia/TerminalApp/Sidebar.cpp`).

| Claude Code event | Sidebar state |
| --- | --- |
| `UserPromptSubmit` | working |
| `PermissionRequest`, `Notification` (`permission_prompt`, `elicitation_*`), `PreToolUse` for `AskUserQuestion` / `ExitPlanMode`, `Elicitation` | waiting |
| `PostToolUse`, `PostToolUseFailure`, answering the prompt in the pane (Enter, Esc, 1–9, y/n) | back to working |
| `Stop` | idle (shows "Done" until you look at the pane) |
| `Notification` `idle_prompt` | idle (fallback when a turn was interrupted, since `Stop` doesn't fire then) |
| `StopFailure` | error |
| `Stop` / `SubagentStop` `background_tasks`, background `Bash`/`Agent`/`Monitor` launches | background tasks |
| `SubagentStart` / `SubagentStop` | subagent count |
| `SessionEnd` | removed |

The hooks run with `"async": true`, so they never slow Claude Code down. A status file whose Claude Code
process has exited is ignored.

## Working directory of plain shells

A shell can't report its directory unless it tells the terminal (OSC 9;9). `install.js` adds a few lines
to `~/.bashrc` (Git Bash) and to PowerShell 7's `profile.ps1` to do that on every prompt. Without them,
plain shells are grouped by their profile's starting directory. Claude Code sessions always report their
own directory.

## Setup

```powershell
node tools/claude-sidebar/install.js            # hooks + shell integration (backs up every file it touches)
node tools/claude-sidebar/install.js --uninstall
pwsh -File tools/claude-sidebar/Deploy-Terminal.ps1 -Build   # build and install "Terminal Dev"
```

Debugging: set `WT_SIDEBAR_HOOK_DEBUG=1` before starting `claude` to log every hook call to
`~/.claude/terminal-status/hook-debug.log`.
