[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$Archive
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Release.ps1')
$root = (Resolve-Path -LiteralPath $PackageRoot).Path
$reports = [IO.Path]::GetFullPath($OutputDirectory)
if ($reports -eq $root -or $reports.StartsWith($root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Keep scan reports outside the release package.' }
New-Item -ItemType Directory -Path $reports -Force | Out-Null
$status = Get-MpComputerStatus
$status | Select-Object AMServiceEnabled, AntivirusEnabled, RealTimeProtectionEnabled, BehaviorMonitorEnabled,
    IoavProtectionEnabled, AMProductVersion, AMEngineVersion, AntivirusSignatureVersion, AntivirusSignatureLastUpdated |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $reports 'defender-status.json') -Encoding utf8
if (-not $status.AntivirusEnabled -or -not $status.RealTimeProtectionEnabled) { throw 'Microsoft Defender must be enabled for the release scan.' }
$tool = Join-Path $env:ProgramData 'Microsoft/Windows Defender/Platform/CurrentVersion/MpCmdRun.exe'
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
    $tool = Join-Path $env:ProgramFiles 'Windows Defender/MpCmdRun.exe'
}
if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw 'Microsoft Defender command-line scanner is unavailable.' }
$targets = @((Get-VeOwnedBinaries $root).FullName) + @($root)
if ($Archive) { $targets += (Resolve-Path -LiteralPath $Archive).Path }
$results = [Collections.Generic.List[object]]::new()
$failed = $false
foreach ($target in $targets) {
    $index = $results.Count + 1
    $before = if (Test-Path -LiteralPath $target -PathType Leaf) { (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash } else { $null }
    $started = [DateTime]::UtcNow
    # The documented custom-scan option prevents remediation and ignores file
    # exclusions. No exclusions, policy changes, quarantine restore or submission.
    $scanOutput = @(& $tool -Scan -ScanType 3 -File $target -DisableRemediation 2>&1)
    $code = $LASTEXITCODE
    $scanOutput | Set-Content -LiteralPath (Join-Path $reports "defender-scan-$index.txt") -Encoding utf8
    $after = if (Test-Path -LiteralPath $target -PathType Leaf) { (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash } else { $null }
    $text = $scanOutput -join "`n"
    $clean = $code -eq 0 -and $text -match 'found no threats' -and $before -eq $after
    if (-not $clean) { $failed = $true }
    $results.Add([pscustomobject]@{
        target = $target; sha256 = $before; sha256_after = $after; started_utc = $started.ToString('o')
        completed_utc = [DateTime]::UtcNow.ToString('o'); exit_code = $code; clean_confirmed = $clean
        output_file = "defender-scan-$index.txt"; arguments = @('-Scan', '-ScanType', '3', '-File', $target, '-DisableRemediation')
    })
}
[ordered]@{ engine = $status.AMEngineVersion; intelligence = $status.AntivirusSignatureVersion; scanner = $tool; targets = @($results) } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $reports 'defender-scans.json') -Encoding utf8
if ($failed) { throw "Defender did not confirm all targets clean. Review $reports/defender-scans.json and the exact scanner output. This scan set did not pass; do not publish an archive with an inconclusive result." }
Write-Host "Defender confirmed $($targets.Count) custom scans clean; version, hashes and exact output recorded in $reports."
