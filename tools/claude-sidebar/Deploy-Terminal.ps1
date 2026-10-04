#Requires -Version 7
<#
.SYNOPSIS
    Builds (optionally) and installs this fork of Windows Terminal as the
    "Windows Terminal Dev" package, side by side with the Store version.

.PARAMETER Build
    Build the CascadiaPackage first.

.PARAMETER Configuration
    Release (default) or Debug.

.NOTES
    Registering a loose (unsigned) package requires Windows Developer Mode.
    On first install, your Store Windows Terminal settings.json is copied over
    so the fork looks and behaves the same.
#>
param(
    [switch]$Build,
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path

if ($Build) {
    Import-Module "$root\tools\OpenConsole.psm1"
    Set-MsBuildDevEnvironment | Out-Null
    $vcpkg = "$root\dep\vcpkg"
    $extra = @()
    if (Test-Path "$vcpkg\vcpkg.exe") {
        # The vcpkg bundled with older Visual Studio 2026 builds doesn't recognize VS 18.
        $extra += "/p:VcpkgRoot=$vcpkg"
    }
    & msbuild.exe "$root\OpenConsole.slnx" "/p:Configuration=$Configuration" /p:Platform=x64 /p:AppxSymbolPackageEnabled=false @extra '/t:Terminal\CascadiaPackage' /m /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
}

$devMode = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction SilentlyContinue
if (-not $devMode -or $devMode.AllowDevelopmentWithoutDevLicense -ne 1) {
    throw 'Developer Mode is off. Enable it in Settings > System > For developers, then re-run.'
}

$packages = "$root\src\cascadia\CascadiaPackage\AppPackages"
# Release builds are named CascadiaPackage_<ver>_x64.msix, Debug ones ..._x64_Debug.msix.
$pattern = if ($Configuration -eq 'Debug') { 'CascadiaPackage_*_x64_Debug.msix' } else { 'CascadiaPackage_*_x64.msix' }
$msix = Get-ChildItem $packages -Recurse -Filter $pattern -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $msix) {
    throw "No x64 $Configuration package found under $packages. Run with -Build."
}
Write-Host "Package: $($msix.FullName)"

$sdkBin = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*" -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\makeappx.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $sdkBin) { throw 'makeappx.exe not found (Windows SDK missing?)' }

$loose = Join-Path $msix.Directory.Parent.FullName "loose-$Configuration"
if (Test-Path $loose) { Remove-Item $loose -Recurse -Force }
& $sdkBin unpack /o /p $msix.FullName /d $loose | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makeappx unpack failed ($LASTEXITCODE)" }

# Frameworks the package depends on (VCLibs etc.) are shipped next to the msix.
$deps = Get-ChildItem (Join-Path $msix.Directory.FullName 'Dependencies\x64') -Filter *.appx -ErrorAction SilentlyContinue
foreach ($dep in $deps) {
    try {
        Add-AppxPackage -Path $dep.FullName -ErrorAction Stop
        Write-Host "Installed dependency $($dep.Name)"
    } catch {
        # Already installed (same or newer version).
    }
}

$existing = Get-AppxPackage -Name 'WindowsTerminalDev*'
if ($existing) {
    Write-Host "Removing previous $($existing.PackageFullName)"
    Get-Process WindowsTerminal -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($existing.InstallLocation, [StringComparison]::OrdinalIgnoreCase) } |
        Stop-Process -Force
    Remove-AppxPackage $existing.PackageFullName -PreserveApplicationData
}

Add-AppxPackage -Register (Join-Path $loose 'AppxManifest.xml') -ForceUpdateFromAnyVersion -ForceApplicationShutdown
$installed = Get-AppxPackage -Name 'WindowsTerminalDev*'
Write-Host "Installed $($installed.PackageFullName)"

# Start from the user's Store Terminal settings on first install.
$devSettings = Join-Path $env:LOCALAPPDATA "Packages\$($installed.PackageFamilyName)\LocalState\settings.json"
$storeSettings = Join-Path $env:LOCALAPPDATA 'Packages\Microsoft.WindowsTerminal_8wekyb3d8bbwe\LocalState\settings.json'
if (-not (Test-Path $devSettings) -and (Test-Path $storeSettings)) {
    New-Item -ItemType Directory -Force (Split-Path $devSettings) | Out-Null
    Copy-Item $storeSettings $devSettings
    Write-Host "Copied your Windows Terminal settings to $devSettings"
}

Write-Host "Launch it from the Start menu as 'Terminal Dev', or run: explorer.exe shell:AppsFolder\$($installed.PackageFamilyName)!App"
