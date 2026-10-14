[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$CertificateThumbprint,
    [string]$TimestampServer = 'http://timestamp.digicert.com',
    [switch]$MachineCertificateStore
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Release.ps1')
$root = (Resolve-Path -LiteralPath $PackageRoot).Path
$files = @(Get-VeOwnedBinaries $root)
$store = if ($MachineCertificateStore) { 'LocalMachine' } else { 'CurrentUser' }
$certificate = Get-Item -LiteralPath "Cert:\$store\My\$CertificateThumbprint" -ErrorAction Stop
if (-not $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date) -or
    $certificate.NotBefore -gt (Get-Date)) { throw 'An active code-signing certificate with its private key is required.' }
if (-not @($certificate.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq '1.3.6.1.5.5.7.3.3' }).Count) {
    throw 'The selected certificate is not intended for code signing.'
}
$chain = [Security.Cryptography.X509Certificates.X509Chain]::new()
try {
    if ($certificate.Subject -eq $certificate.Issuer -or -not $chain.Build($certificate)) {
        throw 'A publicly trusted certificate is required. Self-signed and untrusted certificates do not establish publisher trust.'
    }
} finally { $chain.Dispose() }
$timestampUri = [Uri]$TimestampServer
if (-not $timestampUri.IsAbsoluteUri -or $timestampUri.Scheme -notin @('http', 'https')) {
    throw 'TimestampServer must be an absolute HTTP or HTTPS RFC 3161 timestamp URL.'
}
$tool = Get-VeSignTool
foreach ($file in $files) {
    $arguments = @('sign', '/sha1', $CertificateThumbprint, '/s', 'My', '/fd', 'SHA256',
        '/tr', $TimestampServer, '/td', 'SHA256')
    if ($MachineCertificateStore) { $arguments += '/sm' }
    $arguments += $file.FullName
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) { throw "Signing failed: $($file.Name)" }
    & $tool verify /pa /all /tw $file.FullName
    if ($LASTEXITCODE -ne 0) { throw "Signature/timestamp verification failed: $($file.Name)" }
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($signature.Status -ne 'Valid' -or -not $signature.TimeStamperCertificate -or
        $signature.SignerCertificate.Thumbprint -ne $CertificateThumbprint) {
        throw "Trusted, timestamped publisher signature was not verified: $($file.Name)"
    }
}
# Verify the final signed bytes without adding reports to the runtime package.
& (Join-Path $PSScriptRoot 'audit-release.ps1') -PackageRoot $root -RequireSignatures
