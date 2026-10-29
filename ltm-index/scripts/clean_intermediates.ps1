param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$rootPrefix = $projectRoot + '\'
$reportRoot = Join-Path $projectRoot 'report\maintenance'
$stamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$outputDir = Join-Path $reportRoot ('cleanup_' + $stamp)
New-Item -ItemType Directory -Path $outputDir | Out-Null

function Get-PlainFiles([string]$Directory) {
    $resolved = [System.IO.Path]::GetFullPath($Directory)
    if (-not $resolved.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Path outside project' }
    $folder = Get-Item -LiteralPath $resolved
    if (($folder.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { return }
    foreach ($item in Get-ChildItem -LiteralPath $resolved -Force) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
        if ($item.PSIsContainer) { Get-PlainFiles $item.FullName } else { $item }
    }
}

# Treat hash-addressed files as evidence, even if a future manifest lists an object file.
$protected = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Add-EvidencePaths($Value) {
    if ($Value -is [System.Collections.IDictionary]) {
        foreach ($key in $Value.Keys) {
            $valueAtKey = $Value[$key]
            if ($valueAtKey -is [string] -and $valueAtKey -match '^[0-9a-fA-F]{64}$') {
                try {
                    $candidate = [IO.Path]::GetFullPath((Join-Path $projectRoot $key))
                    if ($candidate.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { [void]$protected.Add($candidate) }
                } catch { }
            } else { Add-EvidencePaths $valueAtKey }
        }
    } elseif ($Value -is [System.Collections.IEnumerable] -and $Value -isnot [string]) {
        foreach ($element in $Value) { Add-EvidencePaths $element }
    }
}
foreach ($file in Get-PlainFiles (Join-Path $projectRoot 'report')) {
    if ($file.Extension -eq '.json') {
        try { Add-EvidencePaths (Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable) }
        catch { throw "Cannot inspect evidence manifest: $($file.FullName)" }
    }
}

$candidates = [Collections.Generic.List[object]]::new()
foreach ($build in Get-ChildItem -LiteralPath $projectRoot -Directory -Filter 'build-*') {
    if (($build.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { continue }
    $cmakeFiles = Join-Path $build.FullName 'CMakeFiles'
    if (Test-Path -LiteralPath $cmakeFiles -PathType Container) {
        foreach ($file in Get-PlainFiles $cmakeFiles) {
            if ($file.Extension -in @('.obj', '.o', '.pdb', '.ilk') -and -not $protected.Contains($file.FullName)) {
                $candidates.Add([ordered]@{ path=$file.FullName; bytes=$file.Length; reason='rebuildable compiler intermediate' })
            }
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $build.FullName -File -Filter '*.ilk') {
        if (-not $protected.Contains($file.FullName)) {
            $candidates.Add([ordered]@{ path=$file.FullName; bytes=$file.Length; reason='rebuildable incremental linker cache' })
        }
    }
}
$pythonCache = Join-Path $projectRoot 'scripts\__pycache__'
if (Test-Path -LiteralPath $pythonCache -PathType Container) {
    foreach ($file in Get-PlainFiles $pythonCache) {
        if ($file.Extension -eq '.pyc' -and -not $protected.Contains($file.FullName)) {
            $candidates.Add([ordered]@{ path=$file.FullName; bytes=$file.Length; reason='rebuildable Python bytecode' })
        }
    }
}
$totalBytes = 0L
foreach ($entry in $candidates) { $totalBytes += $entry['bytes'] }
$manifest = [ordered]@{ applied=[bool]$Apply; project=$projectRoot; protected_paths=$protected.Count;
    candidates=$candidates; candidate_bytes=$totalBytes;
    deleted_count=0; deleted_bytes=0; complete=$false }
$manifestPath = Join-Path $outputDir 'manifest.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
foreach ($entry in $candidates) {
    if (-not $Apply) { continue }
    $absolute = [IO.Path]::GetFullPath($entry.path)
    if (-not $absolute.StartsWith($rootPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refuse deletion outside project' }
    $file = Get-Item -LiteralPath $absolute
    if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Refuse special path' }
    if ($protected.Contains($absolute) -or $file.Length -ne $entry.bytes) { throw 'Candidate changed after inspection' }
    Remove-Item -LiteralPath $absolute -Force
    $manifest.deleted_count += 1
    $manifest.deleted_bytes += $entry.bytes
}
$manifest.complete = $true
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding utf8
Write-Output ('CLEANUP_RESULT: files={0} candidate_mib={1:N1} deleted_mib={2:N1} applied={3}' -f $candidates.Count, ($manifest.candidate_bytes / 1MB), ($manifest.deleted_bytes / 1MB), [bool]$Apply)
Write-Output ('CLEANUP_MANIFEST: ' + $manifestPath)
