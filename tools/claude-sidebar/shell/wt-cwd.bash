# Report the current directory to Windows Terminal (OSC 9;9), so its sidebar
# can group this terminal by git repository and worktree.
if [ -n "$WT_SESSION" ]; then
    __wt_report_cwd() { printf '\e]9;9;"%s"\e\' "$(cygpath -w "$PWD" 2>/dev/null || pwd -W 2>/dev/null || pwd)"; }
    case ";$PROMPT_COMMAND;" in
        *";__wt_report_cwd;"*) ;;
        *) PROMPT_COMMAND="__wt_report_cwd${PROMPT_COMMAND:+;$PROMPT_COMMAND}" ;;
    esac
fi
