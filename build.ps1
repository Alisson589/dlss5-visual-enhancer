[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [string]$QtRoot,
    [string]$BuildDir = 'out',
    [switch]$Test,
    [switch]$Deploy,
    [string]$Destination,
    [string]$NativeBuildRoot,
    [string]$CertificateThumbprint,
    [string]$TimestampServer = 'http://timestamp.digicert.com',
    [switch]$MachineCertificateStore
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools/Toolchain.ps1')
$resolvedQt = Resolve-VeQtRoot $QtRoot
$resolvedBuild = Resolve-VePath $BuildDir $PSScriptRoot
$generator = Get-VeVisualStudioGenerator
$testOption = if ($Test) { 'ON' } else { 'OFF' }
Write-Host "Building C++23 with $generator and Qt at $resolvedQt"
Invoke-VeCommand 'cmake' @('-S', $PSScriptRoot, '-B', $resolvedBuild, '-G', $generator, '-A', 'x64',
    "-DCMAKE_PREFIX_PATH=$resolvedQt", "-DBUILD_TESTING=$testOption")
Invoke-VeCommand 'cmake' @('--build', $resolvedBuild, '--config', $Configuration, '--parallel', '8')
if ($Test) {
    $originalPath = $env:PATH
    try {
        $env:PATH = (Join-Path $resolvedQt 'bin') + ';' + $originalPath
        Invoke-VeCommand 'ctest' @('--test-dir', $resolvedBuild, '-C', $Configuration, '--output-on-failure')
    } finally { $env:PATH = $originalPath }
}
if ($Deploy) {
    & (Join-Path $PSScriptRoot 'tools/deploy.ps1') -BuildDir $resolvedBuild -QtRoot $resolvedQt `
        -Configuration $Configuration -Destination $Destination -NativeBuildRoot $NativeBuildRoot `
        -CertificateThumbprint $CertificateThumbprint `
        -TimestampServer $TimestampServer -MachineCertificateStore:$MachineCertificateStore
}
