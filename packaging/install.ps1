param(
    [string]$Prefix,
    [switch]$AddToPath,
    [switch]$InstallService,
    [switch]$StartService,
    [string[]]$ServiceArgs = @()
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Prefix)) {
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $Prefix = Join-Path $env:LOCALAPPDATA "Programs\mirage"
    } elseif (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        $Prefix = Join-Path $env:USERPROFILE ".mirage"
    } else {
        throw "set -Prefix or LOCALAPPDATA before installing"
    }
}

$PackageDir = Split-Path -Parent $PSCommandPath
$SourceBin = Join-Path $PackageDir "bin"
$SourceExe = Join-Path $SourceBin "mirage.exe"
$SourceShare = Join-Path $PackageDir "share"

if (!(Test-Path $SourceExe)) {
    throw "package binary missing: $SourceExe"
}

$TargetBin = Join-Path $Prefix "bin"
$TargetShare = Join-Path $Prefix "share"
New-Item -ItemType Directory -Force -Path $TargetBin | Out-Null
New-Item -ItemType Directory -Force -Path $TargetShare | Out-Null

Copy-Item -Path (Join-Path $SourceBin "*") -Destination $TargetBin -Recurse -Force
if (Test-Path $SourceShare) {
    Copy-Item -Path (Join-Path $SourceShare "*") -Destination $TargetShare -Recurse -Force
}

$Installed = Join-Path $TargetBin "mirage.exe"
& $Installed --version | Out-Null

if ($AddToPath) {
    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    $PathParts = @()
    if (![string]::IsNullOrWhiteSpace($UserPath)) {
        $PathParts = $UserPath -split ';' | Where-Object { $_ -ne "" }
    }
    if ($PathParts -notcontains $TargetBin) {
        $NewPath = (($PathParts + $TargetBin) -join ';')
        [Environment]::SetEnvironmentVariable("Path", $NewPath, "User")
    }
    if (($env:Path -split ';') -notcontains $TargetBin) {
        $env:Path = "$env:Path;$TargetBin"
    }
}

if ($InstallService -or $StartService) {
    & $Installed service install @ServiceArgs
    if ($LASTEXITCODE -ne 0) {
        throw "service install failed"
    }
}

if ($StartService) {
    & $Installed service start
    if ($LASTEXITCODE -ne 0) {
        throw "service start failed"
    }
}

Write-Host "installed mirage to $Installed"
if ($InstallService -or $StartService) {
    Write-Host "windows service installed"
}
if (-not $AddToPath) {
    Write-Host "run with:"
    Write-Host "  $Installed"
    Write-Host "or reinstall with -AddToPath to make 'mirage' available in new terminals."
}
