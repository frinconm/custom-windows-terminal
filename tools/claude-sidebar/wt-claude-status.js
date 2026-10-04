#!/usr/bin/env node
// Claude Code hook that reports the session's state to the Windows Terminal
// sidebar.
//
// Windows Terminal gives every pane a unique WT_SESSION id, which Claude Code
// (and therefore this hook) inherits. The state is written to
//   %USERPROFILE%\.claude\terminal-status\<WT_SESSION>.json
// and the sidebar (src/cascadia/TerminalApp/Sidebar.cpp) polls that file for
// every pane it shows.
//
// File format (all fields optional except state/ts):
//   {
//     "v": 1,
//     "state": "working" | "waiting" | "idle" | "error",
//     "detail": "Approve Bash: npm test",
//     "cwd": "E:\\repo",
//     "ts": 1759600000000,          // ms since epoch of the last state change
//     "pid": 1234,                  // Claude Code's pid, to detect crashed sessions
//     "subagents": 2,               // foreground subagents currently running
//     "backgroundTasks": [ { "id": "...", "type": "shell", "description": "npm run dev" } ]
//   }
//
// Install with `node install.js` (registers this script for the relevant hook
// events in ~/.claude/settings.json).

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');

// Captured first, so that events are ordered by when Claude Code fired them
// rather than by when their (async) hook process got around to writing.
const startedAt = Date.now();

const wtSession = (process.env.WT_SESSION || '').toLowerCase();
if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(wtSession)) {
    // Not running inside Windows Terminal: nothing to report.
    process.exit(0);
}

const statusDir = path.join(os.homedir(), '.claude', 'terminal-status');
const statusFile = path.join(statusDir, `${wtSession}.json`);
const lockFile = `${statusFile}.lock`;

// Never let a stuck hook linger.
setTimeout(() => process.exit(0), 5000).unref();

let raw = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => {
    raw += chunk;
});
process.stdin.on('end', () => {
    try {
        handle(raw ? JSON.parse(raw) : {});
    } catch (err) {
        debugLog(`error: ${err && err.stack ? err.stack : err}`);
    }
    process.exit(0);
});

function debugLog(message) {
    if (!process.env.WT_SIDEBAR_HOOK_DEBUG) {
        return;
    }
    try {
        fs.mkdirSync(statusDir, { recursive: true });
        fs.appendFileSync(path.join(statusDir, 'hook-debug.log'), `${new Date().toISOString()} ${wtSession} ${message}\n`);
    } catch {
        // ignore
    }
}

function sleep(ms) {
    Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

function withLock(fn) {
    fs.mkdirSync(statusDir, { recursive: true });
    let fd = null;
    for (let attempt = 0; attempt < 80 && fd === null; attempt++) {
        try {
            fd = fs.openSync(lockFile, 'wx');
        } catch (err) {
            if (err.code !== 'EEXIST') {
                throw err;
            }
            try {
                // A hook that died while holding the lock.
                if (Date.now() - fs.statSync(lockFile).mtimeMs > 3000) {
                    fs.unlinkSync(lockFile);
                    continue;
                }
            } catch {
                // The lock went away between our open and stat. Retry.
            }
            sleep(25);
        }
    }
    try {
        return fn();
    } finally {
        if (fd !== null) {
            fs.closeSync(fd);
            try {
                fs.unlinkSync(lockFile);
            } catch {
                // ignore
            }
        }
    }
}

function readStatus() {
    try {
        return JSON.parse(fs.readFileSync(statusFile, 'utf8'));
    } catch {
        return null;
    }
}

function writeStatus(status) {
    const tmp = `${statusFile}.${process.pid}.tmp`;
    fs.writeFileSync(tmp, JSON.stringify(status));
    for (let attempt = 0; ; attempt++) {
        try {
            fs.renameSync(tmp, statusFile);
            return;
        } catch (err) {
            // The terminal may be reading the file right now.
            if (attempt >= 20 || (err.code !== 'EPERM' && err.code !== 'EBUSY' && err.code !== 'EACCES')) {
                try {
                    fs.unlinkSync(tmp);
                } catch {
                    // ignore
                }
                throw err;
            }
            sleep(15);
        }
    }
}

function truncate(text, max) {
    const oneLine = String(text || '').replace(/\s+/g, ' ').trim();
    return oneLine.length > max ? `${oneLine.slice(0, max - 1)}\u2026` : oneLine;
}

// A short description of what a tool call is about to do, for the
// "needs approval" line.
function describeTool(toolName, toolInput) {
    const input = toolInput || {};
    const subject =
        input.description ||
        input.command ||
        input.file_path ||
        input.notebook_path ||
        input.url ||
        input.pattern ||
        input.prompt ||
        input.query ||
        '';
    const name = String(toolName || 'tool').replace(/^mcp__/, '').replace(/__/g, ' ');
    return truncate(subject ? `Approve ${name}: ${subject}` : `Approve ${name}`, 90);
}

function normalizeBackgroundTasks(tasks) {
    if (!Array.isArray(tasks)) {
        return null;
    }
    return tasks
        .filter((t) => t && (!t.status || /running|pending|in_progress|active/i.test(String(t.status))))
        .map((t) => ({
            id: String(t.id || ''),
            type: String(t.type || 'task'),
            description: truncate(t.description || t.command || t.name || t.agent_type || '', 120),
        }));
}

function backgroundLaunch(input) {
    const toolInput = input.tool_input || {};
    const response = input.tool_response || {};
    let type = null;
    if (input.tool_name === 'Monitor') {
        type = 'monitor';
    } else if (toolInput.run_in_background === true) {
        type = input.tool_name === 'Bash' || input.tool_name === 'PowerShell' ? 'shell' : input.tool_name === 'Agent' ? 'subagent' : String(input.tool_name).toLowerCase();
    }
    if (!type) {
        return null;
    }
    const id =
        response.backgroundTaskId ||
        response.taskId ||
        response.task_id ||
        response.shellId ||
        response.agentId ||
        input.tool_use_id ||
        `${startedAt}`;
    return {
        id: String(id),
        type,
        description: truncate(toolInput.description || toolInput.command || toolInput.prompt || '', 120),
    };
}

function handle(input) {
    const event = input.hook_event_name || process.argv[2] || '';
    debugLog(`${event} ${input.notification_type || input.tool_name || ''}`);

    if (event === 'SessionEnd') {
        withLock(() => {
            try {
                fs.unlinkSync(statusFile);
            } catch {
                // ignore
            }
        });
        return;
    }

    withLock(() => {
        const previous = readStatus();
        const fresh = !previous || previous.sessionId !== input.session_id || event === 'SessionStart';
        const status = fresh
            ? { v: 1, state: 'idle', detail: '', subagentIds: [], backgroundTasks: [] }
            : Object.assign({ subagentIds: [], backgroundTasks: [] }, previous);

        // An event older than what's already recorded (async hooks can finish
        // out of order) may still update the task lists, but not the state.
        const stale = !fresh && typeof previous.lastEvent === 'number' && previous.lastEvent > startedAt;
        let changed = fresh;
        const setState = (state, detail) => {
            if (!stale && (status.state !== state || status.detail !== (detail || ''))) {
                status.state = state;
                status.detail = detail || '';
                changed = true;
            }
        };

        switch (event) {
            case 'SessionStart':
                setState('idle');
                if (input.source === 'compact' && previous) {
                    status.backgroundTasks = previous.backgroundTasks || [];
                }
                cleanupOldFiles();
                break;

            case 'UserPromptSubmit':
                setState('working');
                break;

            case 'PreToolUse':
                // Registered only for tools that block on the user (see install.js).
                if (input.tool_name === 'AskUserQuestion') {
                    const questions = (input.tool_input && input.tool_input.questions) || [];
                    const first = questions[0] && (questions[0].question || questions[0].header);
                    setState('waiting', truncate(first ? `Question: ${first}` : 'Claude has a question', 90));
                } else if (input.tool_name === 'ExitPlanMode') {
                    setState('waiting', 'Plan ready for review');
                }
                break;

            case 'PermissionRequest':
                // Questions and plans already got a better description from PreToolUse.
                if (!(status.state === 'waiting' && /^(AskUserQuestion|ExitPlanMode)$/.test(input.tool_name || ''))) {
                    setState('waiting', describeTool(input.tool_name, input.tool_input));
                }
                break;

            case 'Elicitation':
                setState('waiting', truncate(`${input.mcp_server_name || 'MCP server'} needs input`, 90));
                break;

            case 'ElicitationResult':
                if (status.state === 'waiting') {
                    setState('working');
                }
                break;

            case 'Notification':
                switch (input.notification_type) {
                    case 'permission_prompt':
                    case 'elicitation_dialog':
                    case 'elicitation_url_dialog':
                    case 'agent_needs_input':
                        // Keep the more specific text PermissionRequest wrote.
                        if (status.state !== 'waiting' || !status.detail) {
                            setState('waiting', truncate(input.message || 'Needs your input', 90));
                        }
                        break;
                    case 'idle_prompt':
                        // Stop doesn't fire when the user interrupts a turn;
                        // this is the fallback that clears "working".
                        if (status.state === 'working') {
                            setState('idle');
                        }
                        break;
                    default:
                        break;
                }
                break;

            case 'PostToolUse':
            case 'PostToolUseFailure': {
                if (status.state === 'waiting' || status.state === 'idle') {
                    setState('working');
                }
                const launch = event === 'PostToolUse' ? backgroundLaunch(input) : null;
                if (launch && !status.backgroundTasks.some((t) => t.id === launch.id)) {
                    status.backgroundTasks.push(launch);
                }
                break;
            }

            case 'SubagentStart':
                if (input.agent_id && !status.subagentIds.includes(input.agent_id)) {
                    status.subagentIds.push(input.agent_id);
                }
                break;

            case 'SubagentStop': {
                status.subagentIds = status.subagentIds.filter((id) => id !== input.agent_id);
                const tasks = normalizeBackgroundTasks(input.background_tasks);
                if (tasks) {
                    status.backgroundTasks = tasks;
                }
                break;
            }

            case 'Stop': {
                setState('idle');
                status.subagentIds = [];
                const tasks = normalizeBackgroundTasks(input.background_tasks);
                if (tasks) {
                    status.backgroundTasks = tasks;
                }
                break;
            }

            case 'StopFailure':
                setState('error', truncate(input.error || input.message || 'API error', 90));
                status.subagentIds = [];
                break;

            default:
                break;
        }

        status.v = 1;
        status.sessionId = input.session_id || status.sessionId || '';
        status.wtSession = wtSession;
        status.cwd = input.cwd || status.cwd || '';
        status.pid = process.ppid;
        status.event = event;
        status.subagents = status.subagentIds.length;
        if (!stale) {
            status.lastEvent = startedAt;
        }
        // ts only moves when the state does: the terminal uses it to tell
        // whether the user has seen (or answered) the current state.
        if (changed || typeof status.ts !== 'number') {
            status.ts = startedAt;
        }
        writeStatus(status);
    });
}

// Remove status files of panes/sessions that are long gone.
function cleanupOldFiles() {
    try {
        const cutoff = Date.now() - 3 * 24 * 60 * 60 * 1000;
        for (const name of fs.readdirSync(statusDir)) {
            if (!/\.(json|tmp|lock)$/.test(name)) {
                continue;
            }
            const file = path.join(statusDir, name);
            try {
                if (fs.statSync(file).mtimeMs < cutoff) {
                    fs.unlinkSync(file);
                }
            } catch {
                // ignore
            }
        }
    } catch {
        // ignore
    }
}
