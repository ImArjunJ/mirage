param(
    [string]$Triplet = "x64-windows",
    [int]$HeartbeatSeconds = 60
)

$ErrorActionPreference = "Stop"

function Format-Bytes {
    param([double]$Bytes)

    if ($Bytes -ge 1GB) {
        return "{0:N1} GB" -f ($Bytes / 1GB)
    }

    if ($Bytes -ge 1MB) {
        return "{0:N1} MB" -f ($Bytes / 1MB)
    }

    if ($Bytes -ge 1KB) {
        return "{0:N1} KB" -f ($Bytes / 1KB)
    }

    return "{0:N0} B" -f $Bytes
}

function Write-DirectoryStats {
    param(
        [string]$Label,
        [string]$Path
    )

    if (!(Test-Path $Path)) {
        Write-Host "$Label: missing ($Path)"
        return
    }

    $stats = Get-ChildItem -Path $Path -Recurse -File -ErrorAction SilentlyContinue |
        Measure-Object -Property Length -Sum
    $count = $stats.Count
    $bytes = 0
    if ($null -ne $stats.Sum) {
        $bytes = $stats.Sum
    }

    Write-Host "$Label: $count files, $(Format-Bytes $bytes) ($Path)"
}

$workspace = $env:GITHUB_WORKSPACE
if ([string]::IsNullOrWhiteSpace($workspace)) {
    $workspace = (Get-Location).Path
}

$cache = Join-Path $workspace ".vcpkg-binary-cache"
$installed = Join-Path $workspace "vcpkg_installed"

New-Item -ItemType Directory -Force $cache | Out-Null
$env:VCPKG_BINARY_SOURCES = "clear;files,$cache,readwrite"

Write-Host "vcpkg root: $env:VCPKG_INSTALLATION_ROOT"
Write-Host "workspace: $workspace"
Write-Host "triplet: $Triplet"
Write-Host "binary sources: $env:VCPKG_BINARY_SOURCES"
Write-DirectoryStats "binary cache before install" $cache
Write-DirectoryStats "installed tree before install" $installed
vcpkg version
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg version failed with exit code $LASTEXITCODE"
}

$started = Get-Date
$nextHeartbeat = $started.AddSeconds($HeartbeatSeconds)

$job = Start-Job -ScriptBlock {
    param(
        [string]$JobTriplet,
        [string]$JobCache,
        [string]$JobWorkspace
    )

    $ErrorActionPreference = "Stop"
    $env:VCPKG_BINARY_SOURCES = "clear;files,$JobCache,readwrite"
    Set-Location $JobWorkspace

    & vcpkg install --triplet $JobTriplet 2>&1 | ForEach-Object {
        $_
    }

    if ($LASTEXITCODE -ne 0) {
        throw "vcpkg install failed with exit code $LASTEXITCODE"
    }
} -ArgumentList $Triplet, $cache, $workspace

try {
    while (($job.State -eq "Running") -or ($job.State -eq "NotStarted")) {
        Receive-Job $job | ForEach-Object {
            Write-Host $_
        }

        $now = Get-Date
        if ($now -ge $nextHeartbeat) {
            $elapsed = [int]($now - $started).TotalSeconds
            Write-Host "::notice::vcpkg install still running after ${elapsed}s"
            Write-DirectoryStats "binary cache during install" $cache
            Write-DirectoryStats "installed tree during install" $installed
            $nextHeartbeat = $now.AddSeconds($HeartbeatSeconds)
        }

        Start-Sleep -Seconds 5
    }

    Receive-Job $job | ForEach-Object {
        Write-Host $_
    }

    if ($job.State -ne "Completed") {
        $reason = $job.ChildJobs[0].JobStateInfo.Reason
        throw "vcpkg install job ended as $($job.State): $reason"
    }
}
finally {
    Remove-Job $job -Force -ErrorAction SilentlyContinue
}

$elapsedTotal = [int]((Get-Date) - $started).TotalSeconds
Write-Host "vcpkg install completed in ${elapsedTotal}s"
Write-DirectoryStats "binary cache after install" $cache
Write-DirectoryStats "installed tree after install" $installed
