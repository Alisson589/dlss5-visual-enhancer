[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$Destination,
    [Parameter(Mandatory)][string]$BackupDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Release.ps1')
$stageRoot = (Resolve-Path -LiteralPath $Stage).Path.TrimEnd('\', '/')
$destinationRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\', '/')
$backupRoot = [IO.Path]::GetFullPath($BackupDirectory).TrimEnd('\', '/')
function Assert-VeSeparatePath([string]$A, [string]$B) {
    if ($A -eq $B -or $A.StartsWith($B + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $B.StartsWith($A + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Stage, destination and backup must be separate dedicated directories.' }
}
Assert-VeSeparatePath $stageRoot $destinationRoot
Assert-VeSeparatePath $stageRoot $backupRoot
Assert-VeSeparatePath $destinationRoot $backupRoot
if ([IO.Path]::GetPathRoot($stageRoot) -ne [IO.Path]::GetPathRoot($destinationRoot) -or
    [IO.Path]::GetPathRoot($backupRoot) -ne [IO.Path]::GetPathRoot($destinationRoot)) { throw 'Promotion and rollback require directories on the same volume.' }
if (Test-Path -LiteralPath $backupRoot) { throw 'The backup directory must not already exist.' }
foreach ($path in @($stageRoot, $destinationRoot)) {
    if (Test-Path -LiteralPath $path) {
        if ((Get-Item -LiteralPath $path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Cannot promote a linked directory: $path" }
        if (@(Get-ChildItem -LiteralPath $path -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) {
            throw "Cannot promote a tree containing filesystem links: $path"
        }
    }
}
$null = @(Get-VeOwnedBinaries $stageRoot)
if ((Test-Path -LiteralPath (Join-Path $stageRoot 'app')) -or (Test-Path -LiteralPath (Join-Path $stageRoot 'data'))) { throw 'The staged package still has the legacy layout.' }
$running = @(Get-Process | Where-Object { $_.Path -and ($_.Path.StartsWith($destinationRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or $_.Path.StartsWith($stageRoot + '\', [StringComparison]::OrdinalIgnoreCase)) })
if ($running.Count) { throw "Close Visual Enhancer and its workers before promotion: $($running.Id -join ', ')" }

$collisions = [Collections.Generic.List[string]]::new()
function Copy-VeUserFiles([string]$Source, [string]$Target) {
    New-Item -ItemType Directory -Path $Target -Force | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        $targetPath = Join-Path $Target $item.Name
        if ($item.PSIsContainer) {
            if (Test-Path -LiteralPath $targetPath -PathType Leaf) { throw "File/directory collision: $targetPath" }
            Copy-VeUserFiles $item.FullName $targetPath
        } else {
            if (Test-Path -LiteralPath $targetPath) {
                if ((Test-Path -LiteralPath $targetPath -PathType Leaf) -and
                    (Get-FileHash -LiteralPath $item.FullName).Hash -eq (Get-FileHash -LiteralPath $targetPath).Hash) { continue }
                $index = 1
                do {
                    $targetPath = Join-Path $Target ($item.BaseName + '-legacy-' + $index++ + $item.Extension)
                } while (Test-Path -LiteralPath $targetPath)
                $collisions.Add($targetPath)
            }
            Copy-Item -LiteralPath $item.FullName -Destination $targetPath
        }
    }
}
foreach ($name in @('outputs', 'logs')) {
    foreach ($relative in @($name, ('data/' + $name))) {
        $source = Join-Path $destinationRoot $relative
        if (Test-Path -LiteralPath $source -PathType Container) { Copy-VeUserFiles $source (Join-Path $stageRoot $name) }
    }
}
New-Item -ItemType Directory -Path (Split-Path -Parent $backupRoot),$OutputDirectory -Force | Out-Null
$hadDestination = Test-Path -LiteralPath $destinationRoot
if ($hadDestination) {
    # Recognize the existing product before moving the entire installation.
    if (-not (Test-Path -LiteralPath (Join-Path $destinationRoot 'Visual Enhancer.exe') -PathType Leaf)) {
        throw 'The destination is not an existing Visual Enhancer installation.'
    }
    Move-Item -LiteralPath $destinationRoot -Destination $backupRoot
}
try {
    Move-Item -LiteralPath $stageRoot -Destination $destinationRoot
} catch {
    if ($hadDestination -and -not (Test-Path -LiteralPath $destinationRoot)) { Move-Item -LiteralPath $backupRoot -Destination $destinationRoot }
    throw
}
# Only a preference equal to this installation's former default is migrated.
# Arbitrary user-selected folders and all other preferences remain unchanged.
$key = 'HKCU:\Software\Merserk\Visual Enhancer\export'
$migration = $false
if (Test-Path -LiteralPath $key) {
    $old = (Get-Item -LiteralPath $key).GetValue('folder', $null)
    $knownDefault = Join-Path $destinationRoot 'data/outputs'
    if ($old -and $old.Replace('/', '\').TrimEnd('\') -eq $knownDefault.Replace('/', '\')) {
        $old | Set-Content -LiteralPath (Join-Path $OutputDirectory 'previous-export-folder.txt') -Encoding utf8
        Set-ItemProperty -LiteralPath $key -Name folder -Value (Join-Path $destinationRoot 'outputs')
        $migration = $true
    }
}
[ordered]@{ installed = $destinationRoot; previous = if ($hadDestination) { $backupRoot } else { $null }; generated_utc = [DateTime]::UtcNow.ToString('o'); export_default_migrated = $migration; collision_copies = @($collisions) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'promotion.json') -Encoding utf8
Write-Host "Previous installation preserved at: $backupRoot"
