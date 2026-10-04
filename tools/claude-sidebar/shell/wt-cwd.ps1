# Report the current directory to Windows Terminal (OSC 9;9), so its sidebar
# can group this terminal by git repository and worktree.
if ($env:WT_SESSION -and -not $global:__WtSidebarPrompt) {
    $global:__WtSidebarPrompt = $function:prompt
    function global:prompt {
        $location = $ExecutionContext.SessionState.Path.CurrentLocation
        $prompt = & $global:__WtSidebarPrompt
        if ($location.Provider.Name -eq 'FileSystem') {
            "$([char]27)]9;9;`"$($location.ProviderPath)`"$([char]27)\" + $prompt
        } else {
            $prompt
        }
    }
}
