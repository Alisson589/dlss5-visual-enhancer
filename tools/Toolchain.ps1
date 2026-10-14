Set-StrictMode -Version Latest

function Test-VeQtKit {
    param([string]$QtRoot)
    $configPath = Join-Path $QtRoot 'lib/cmake/Qt6/Qt6Config.cmake'
    $metadataPath = Join-Path $QtRoot 'mkspecs/qconfig.pri'
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf) -or
        -not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
        return $false
    }
    $metadata = Get-Content -LiteralPath $metadataPath -Raw -ErrorAction Stop
    return ($metadata -match '(?m)^QT_VERSION[ \t]*=[ \t]*6\.12\.\d+[ \t]*\r?$' -and
        $metadata -match '(?m)^QT_ARCH[ \t]*=[ \t]*x86_64[ \t]*\r?$' -and
        $metadata -match '(?m)^QT_MSVC_MAJOR_VERSION[ \t]*=[ \t]*\d+[ \t]*\r?$')
}

function Resolve-VeQtRoot {
    param([string]$QtRoot)
    if (-not $QtRoot) { $QtRoot = [Environment]::GetEnvironmentVariable('VE_QT_ROOT') }
    if ($QtRoot) {
        $resolved = (Resolve-Path -LiteralPath $QtRoot -ErrorAction Stop).Path
        if (-not (Test-VeQtKit $resolved)) {
            throw "QtRoot must name a Qt 6.12.x MSVC x64 kit, for example C:\Qt\6.12.0\msvc2022_64."
        }
        return $resolved
    }
    if (Test-Path -LiteralPath 'C:\Qt') {
        $versions = Get-ChildItem -LiteralPath 'C:\Qt' -Directory |
            Where-Object { $_.Name -match '^6\.12\.\d+$' } |
            Sort-Object { [version]$_.Name } -Descending
        foreach ($version in $versions) {
            $kits = Get-ChildItem -LiteralPath $version.FullName -Directory |
                Where-Object { $_.Name -match '^msvc\d+_64$' } |
                Sort-Object Name -Descending
            foreach ($kit in $kits) {
                if (Test-VeQtKit $kit.FullName) {
                    return $kit.FullName
                }
            }
        }
    }
    throw 'Qt 6.12.x with MSVC x64 is required. Pass -QtRoot or set VE_QT_ROOT.'
}

function Get-VeVisualStudioInstallation {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with Desktop development with C++.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json |
        ConvertFrom-Json | Select-Object -First 1
    if (-not $installation) { throw 'Visual Studio C++ tools were not found.' }
    return $installation
}

function Get-VeVisualStudioGenerator {
    $installation = Get-VeVisualStudioInstallation
    switch (([version]$installation.installationVersion).Major) {
        18 { return 'Visual Studio 18 2026' }
        17 { return 'Visual Studio 17 2022' }
        default { throw 'A recent Visual Studio C++23 compiler is required.' }
    }
}

function Invoke-VeCommand {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE." }
}

function Resolve-VePath {
    param([string]$Path, [string]$BaseDirectory)
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $BaseDirectory $Path))
}
