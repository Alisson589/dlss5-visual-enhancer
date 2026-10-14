[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [string]$OutputDirectory,
    [switch]$RequireSignatures
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Release.ps1')
$root = (Resolve-Path -LiteralPath $PackageRoot).Path
$owned = @(Get-VeOwnedBinaries $root)
$ownedPaths = @($owned.FullName)
$releaseVersion = $owned[0].VersionInfo.ProductVersion
$inventory = foreach ($file in (Get-ChildItem -LiteralPath $root -Recurse -File |
    Where-Object { $_.Extension -in @('.exe', '.dll') } | Sort-Object FullName)) {
    $isOwned = $file.FullName -in $ownedPaths
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    $security = Get-VePeSecurity $file.FullName
    if ($isOwned) {
        if ($file.VersionInfo.CompanyName -ne 'Merserk' -or
            $file.VersionInfo.OriginalFilename -ne $file.Name) { throw "Incorrect application identity: $($file.FullName)" }
        if (-not $security.x64 -or -not $security.aslr -or -not $security.dep -or
            -not $security.high_entropy_aslr -or -not $security.control_flow_guard) {
            throw "Missing Windows security protection: $($file.FullName)"
        }
        if ($file.Extension -eq '.exe' -and $file.VersionInfo.ProductVersion -ne $releaseVersion) {
            throw "Inconsistent application release version: $($file.Name)"
        }
        if ($RequireSignatures -and ($signature.Status -ne 'Valid' -or -not $signature.TimeStamperCertificate)) {
            throw "A trusted, timestamped signature is required: $($file.Name)"
        }
        if ($signature.Status -notin @('Valid', 'NotSigned')) { throw "Invalid application signature: $($file.Name)" }
    }
    [pscustomobject]@{
        path = [IO.Path]::GetRelativePath($root, $file.FullName).Replace('\', '/')
        size_bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        application_owned = $isOwned
        company = $file.VersionInfo.CompanyName
        product = $file.VersionInfo.ProductName
        file_version = $file.VersionInfo.FileVersion
        product_version = $file.VersionInfo.ProductVersion
        signature_status = $signature.Status.ToString()
        signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { $null }
        timestamped = [bool]$signature.TimeStamperCertificate
        windows_security = $security
    }
}
$report = [ordered]@{
    schema_version = 1
    product = 'Visual Enhancer'
    release_version = $releaseVersion
    generated_utc = [DateTime]::UtcNow.ToString('o')
    signatures_required = [bool]$RequireSignatures
    neural_rendering = 'Private NVIDIA interface; existing temporary nvngx.dll caller alias retained for compatibility.'
    binaries = @($inventory)
}
if ($OutputDirectory) {
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'binary-inventory.json') -Encoding utf8
    @($inventory | ForEach-Object { "$($_.sha256)  $($_.path)" }) |
        Set-Content -LiteralPath (Join-Path $OutputDirectory 'SHA256SUMS.txt') -Encoding utf8
    Write-Host "Inventory and SHA-256 hashes: $OutputDirectory"
}
Write-Host "Release audit passed for $($owned.Count) application binaries; $($inventory.Count) binaries inventoried."
