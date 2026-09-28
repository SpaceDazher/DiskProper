# MrProper: serialize builds.
# Why: several agents run MSVC on one host with 7.8 GB RAM. Parallel cl.exe +
# link.exe exhaust memory, the kernel OOM-kills processes, and BB workflows then
# report "Thread interrupted because the connection to the host was lost".
# So compilation runs strictly one at a time. The wait is bounded so a crashed
# agent cannot hold the queue forever.
# NOTE: PowerShell 5.1 reads .ps1 without BOM as ANSI, so this file must stay
# ASCII-only: non-ASCII comments here break the parser.
param(
    [Parameter(Mandatory = $true)][ValidateSet('acquire', 'release')][string]$Action
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$lock = Join-Path $repo 'build\.build-lock'
$waitMinutes = 40
$staleMinutes = 45

if ($Action -eq 'acquire') {
    $deadline = (Get-Date).AddMinutes($waitMinutes)
    while ($true) {
        try {
            New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null
            Set-Content -Path (Join-Path $lock 'owner.txt') -Value "$PID $(Get-Date -Format o)"
            exit 0
        } catch {
            # directory already exists: another agent holds it
        }
        $age = (Get-Date) - (Get-Item $lock).CreationTime
        if ($age.TotalMinutes -gt $staleMinutes) {
            Write-Host "[lock] breaking stale lock (age $([int]$age.TotalMinutes) min)"
            Remove-Item $lock -Recurse -Force -ErrorAction SilentlyContinue
            continue
        }
        if ((Get-Date) -gt $deadline) {
            Write-Host "[lock] queue wait over $waitMinutes min - build not started"
            exit 3
        }
        Start-Sleep -Seconds 5
    }
}

Remove-Item $lock -Recurse -Force -ErrorAction SilentlyContinue
exit 0
