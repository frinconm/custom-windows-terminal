#!/usr/bin/env node
// Installs (or with --uninstall, removes) the Claude Code hooks that feed the
// Windows Terminal sidebar, plus optional shell integration that reports the
// shell's current directory to the terminal (OSC 9;9).
//
//   node install.js                 hooks + shell integration
//   node install.js --hooks-only    just the Claude Code hooks
//   node install.js --uninstall     remove everything this script added
//
// Every file it modifies is backed up next to itself first.

'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFileSync } = require('child_process');

const args = new Set(process.argv.slice(2));
const uninstall = args.has('--uninstall');
const hooksOnly = args.has('--hooks-only');

const home = os.homedir();
const claudeDir = path.join(home, '.claude');
const hooksDir = path.join(claudeDir, 'hooks');
const settingsPath = path.join(claudeDir, 'settings.json');
const hookScriptName = 'wt-claude-status.js';
const installedHook = path.join(hooksDir, hookScriptName);

// Events the sidebar cares about. SessionEnd runs synchronously, because
// Claude Code may not wait for async hooks while it exits.
const EVENTS = [
    'SessionStart',
    'PreToolUse',
    'UserPromptSubmit',
    'PermissionRequest',
    'Notification',
    'PostToolUse',
    'PostToolUseFailure',
    'SubagentStart',
    'SubagentStop',
    'Stop',
    'StopFailure',
    'Elicitation',
    'ElicitationResult',
    'SessionEnd',
];

// PreToolUse runs before every tool call; only the tools that wait for the
// user are interesting (and in bypass-permissions mode they're the only
// thing that blocks on you).
const MATCHERS = {
    PreToolUse: '^(AskUserQuestion|ExitPlanMode)$',
};

const MARK_BEGIN = '# >>> windows-terminal-sidebar >>>';
const MARK_END = '# <<< windows-terminal-sidebar <<<';

function backup(file) {
    if (fs.existsSync(file)) {
        const stamp = new Date().toISOString().replace(/[:.]/g, '-');
        const target = `${file}.bak-${stamp}`;
        fs.copyFileSync(file, target);
        console.log(`  backed up ${file} -> ${target}`);
    }
}

function isOurHook(handler) {
    const all = [handler && handler.command, ...((handler && handler.args) || [])].join(' ');
    return all.includes(hookScriptName);
}

function updateSettings() {
    let settings = {};
    if (fs.existsSync(settingsPath)) {
        const text = fs.readFileSync(settingsPath, 'utf8');
        try {
            settings = text.trim() ? JSON.parse(text) : {};
        } catch (err) {
            throw new Error(`${settingsPath} is not valid JSON (${err.message}); fix it and re-run.`);
        }
    }
    backup(settingsPath);

    settings.hooks = settings.hooks || {};

    // Drop any previous registration first, so re-running is idempotent.
    for (const [event, groups] of Object.entries(settings.hooks)) {
        if (!Array.isArray(groups)) {
            continue;
        }
        const kept = groups
            .map((group) => Object.assign({}, group, { hooks: (group.hooks || []).filter((h) => !isOurHook(h)) }))
            .filter((group) => group.hooks.length > 0);
        if (kept.length > 0) {
            settings.hooks[event] = kept;
        } else {
            delete settings.hooks[event];
        }
    }

    if (!uninstall) {
        for (const event of EVENTS) {
            const handler = {
                type: 'command',
                command: process.execPath,
                args: [installedHook.replace(/\\/g, '/')],
                timeout: event === 'SessionEnd' ? 2 : 10,
            };
            if (event !== 'SessionEnd') {
                handler.async = true;
            }
            const group = { hooks: [handler] };
            if (MATCHERS[event]) {
                group.matcher = MATCHERS[event];
            }
            settings.hooks[event] = settings.hooks[event] || [];
            settings.hooks[event].push(group);
        }
    }

    if (Object.keys(settings.hooks).length === 0) {
        delete settings.hooks;
    }

    fs.mkdirSync(claudeDir, { recursive: true });
    fs.writeFileSync(settingsPath, `${JSON.stringify(settings, null, 2)}\n`);
    console.log(`  ${uninstall ? 'removed hooks from' : 'registered hooks in'} ${settingsPath}`);
}

function installHookScript() {
    fs.mkdirSync(hooksDir, { recursive: true });
    if (uninstall) {
        if (fs.existsSync(installedHook)) {
            fs.unlinkSync(installedHook);
            console.log(`  removed ${installedHook}`);
        }
        return;
    }
    fs.copyFileSync(path.join(__dirname, hookScriptName), installedHook);
    console.log(`  installed ${installedHook}`);
}

function replaceBlock(file, block) {
    let text = fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : '';
    const pattern = new RegExp(`\\r?\\n?${MARK_BEGIN.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}[\\s\\S]*?${MARK_END.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\r?\\n?`);
    const had = pattern.test(text);
    if (!had && !block) {
        return;
    }
    backup(file);
    text = text.replace(pattern, '\n');
    if (block) {
        text = `${text.replace(/\s*$/, '')}${text.trim() ? '\n\n' : ''}${MARK_BEGIN}\n${block}\n${MARK_END}\n`;
    }
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file, text.trim() ? text : '');
    console.log(`  ${block ? 'updated' : 'cleaned'} ${file}`);
}

const readSnippet = (name) => fs.readFileSync(path.join(__dirname, 'shell', name), 'utf8').replace(/\r\n/g, '\n').trim();
const BASH_BLOCK = readSnippet('wt-cwd.bash');
const PWSH_BLOCK = readSnippet('wt-cwd.ps1');

function powershellProfiles() {
    const profiles = [];
    // PowerShell 7 only: Windows PowerShell 5.1 refuses to run profile scripts
    // under its default execution policy and would print an error on every start.
    for (const exe of ['pwsh']) {
        try {
            const out = execFileSync(exe, ['-NoProfile', '-NonInteractive', '-Command', '$PROFILE.CurrentUserAllHosts'], {
                encoding: 'utf8',
                stdio: ['ignore', 'pipe', 'ignore'],
                timeout: 20000,
            }).trim();
            if (out) {
                profiles.push(out);
            }
        } catch {
            // Not installed.
        }
    }
    return profiles;
}

function updateShellIntegration() {
    // Git Bash reads ~/.bash_profile for login shells; make sure it sources ~/.bashrc.
    const bashrc = path.join(home, '.bashrc');
    const bashProfile = path.join(home, '.bash_profile');
    replaceBlock(bashrc, uninstall ? null : BASH_BLOCK);
    if (!uninstall && !fs.existsSync(bashProfile)) {
        fs.writeFileSync(bashProfile, `# generated by windows-terminal-sidebar\ntest -f ~/.bashrc && . ~/.bashrc\n`);
        console.log(`  created ${bashProfile}`);
    }

    for (const profile of powershellProfiles()) {
        replaceBlock(profile, uninstall ? null : PWSH_BLOCK);
    }
}

try {
    console.log(uninstall ? 'Uninstalling Windows Terminal sidebar integration' : 'Installing Windows Terminal sidebar integration');
    installHookScript();
    updateSettings();
    if (!hooksOnly) {
        updateShellIntegration();
    }
    console.log('Done. Restart Claude Code sessions (and shells) for the changes to take effect.');
} catch (err) {
    console.error(`error: ${err.message}`);
    process.exit(1);
}
