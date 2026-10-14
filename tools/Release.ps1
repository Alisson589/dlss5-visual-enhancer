Set-StrictMode -Version Latest

function Get-VeOwnedBinaries([string]$PackageRoot) {
    foreach ($relative in @('Visual Enhancer.exe', 'VE_CLI.exe',
        'app/Visual Enhancer.app.exe', 'app/VE_CLI.app.exe',
        'app/bin/runtime/dlssnr/neuroframe_engine_neural_rendering.dll',
        'app/bin/runtime/dlsssr/neuroframe_engine_super_resolution.dll',
        'app/bin/runtime/dlssg/neuroframe_engine_frame_interpolation.dll',
        'app/bin/runtime/rtx_video/neuroframe_engine_upscaling.dll',
        'app/bin/runtime/vfx/neuroframe_engine_video_effects.dll')) {
        $path = Join-Path $PackageRoot $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing application binary: $relative" }
        Get-Item -LiteralPath $path
    }
}

function Get-VeSignTool {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $kit = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Windows Kits/10/bin'
    if (Test-Path -LiteralPath $kit) {
        foreach ($version in (Get-ChildItem -LiteralPath $kit -Directory |
            Where-Object Name -Match '^\d+\.\d+\.\d+\.\d+$' |
            Sort-Object { [version]$_.Name } -Descending)) {
            $tool = Join-Path $version.FullName 'x64/signtool.exe'
            if (Test-Path -LiteralPath $tool -PathType Leaf) { return $tool }
        }
    }
    throw 'Install the Windows SDK signing tools to sign a release.'
}

function Get-VePeSecurity([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows PE file: $Path" }
        $stream.Position = 0x3C
        $pe = $reader.ReadInt32()
        if ($pe -lt 64 -or $pe -gt ($stream.Length - 96)) { throw "Invalid PE header: $Path" }
        $stream.Position = $pe
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "Invalid PE signature: $Path" }
        $machine = $reader.ReadUInt16()
        $stream.Position = $pe + 24
        $magic = $reader.ReadUInt16()
        $stream.Position = $pe + 24 + 70
        $characteristics = $reader.ReadUInt16()
        [pscustomobject]@{
            x64 = ($machine -eq 0x8664 -and $magic -eq 0x20B)
            aslr = [bool]($characteristics -band 0x40)
            high_entropy_aslr = [bool]($characteristics -band 0x20)
            dep = [bool]($characteristics -band 0x100)
            control_flow_guard = [bool]($characteristics -band 0x4000)
        }
    } finally { $reader.Dispose() }
}
