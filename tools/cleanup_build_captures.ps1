<#
.SYNOPSIS
Removes disposable raw render captures while preserving builds and reports.
.DESCRIPTION
Dry run unless -Apply is supplied. Only .hybrid.bin, .lighting.bin and
.environment.bin files under this repository's build directory are candidates.
Preserves x64-* build trees, ground-truth/dataset* reference directories,
Git-tracked files, reports, logs, preview images, scene assets and Nsight traces.
Does not traverse junctions or symbolic links. Deleted captures must be
regenerated before running analyses that need their raw pixel data.
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\cleanup_build_captures.ps1
.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\cleanup_build_captures.ps1 -Apply
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'Medium')]
param([switch]$Apply)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')).TrimEnd('\')
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build')).TrimEnd('\')
$buildPrefix = $buildRoot + '\'
$buildItem = Get-Item -LiteralPath $buildRoot -Force
if (-not $buildItem.PSIsContainer -or ($buildItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
    throw 'The build directory must be an ordinary directory, not a junction or symbolic link.'
}

# Track exclusions in addition to the filename and directory rules.
$trackedFiles = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$trackedPaths = @(& git -C $repositoryRoot ls-files -- build)
if ($LASTEXITCODE -ne 0) { throw 'Could not read Git-tracked files. No files were deleted.' }
foreach ($trackedPath in $trackedPaths) {
    [void]$trackedFiles.Add([System.IO.Path]::GetFullPath((Join-Path $repositoryRoot $trackedPath)))
}

function Assert-CapturePath {
    param([string]$Path)
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if (-not $fullPath.StartsWith($buildPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Target is outside the build directory: $fullPath"
    }
    $relativePath = $fullPath.Substring($buildPrefix.Length)
    if ($relativePath -match '^(x64-[^\\]+|ground-truth\\dataset[^\\]*)(\\|$)' -or
        $relativePath -notmatch '\.(hybrid|lighting|environment)\.bin$' -or
        $trackedFiles.Contains($fullPath)) {
        throw "Protected or unexpected target: $fullPath"
    }
    $cursor = $fullPath
    while ($true) {
        $item = Get-Item -LiteralPath $cursor -Force
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to follow a junction or symbolic link: $cursor"
        }
        if ($cursor.Equals($buildRoot, [System.StringComparison]::OrdinalIgnoreCase)) { break }
        $cursor = [System.IO.Path]::GetDirectoryName($cursor)
    }
}

# Manual traversal works in Windows PowerShell 5.1 and PowerShell 7.
$directories = [System.Collections.Generic.Stack[string]]::new()
$directories.Push($buildRoot)
$captures = [System.Collections.Generic.List[object]]::new()
while ($directories.Count -gt 0) {
    $directory = $directories.Pop()
    foreach ($item in (Get-ChildItem -LiteralPath $directory -Force)) {
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { continue }
        $relativePath = $item.FullName.Substring($buildPrefix.Length)
        if ($relativePath -match '^(x64-[^\\]+|ground-truth\\dataset[^\\]*)(\\|$)') { continue }
        if ($item.PSIsContainer) {
            $directories.Push($item.FullName)
        }
        elseif ($item.Name -match '\.(hybrid|lighting|environment)\.bin$' -and -not $trackedFiles.Contains($item.FullName)) {
            Assert-CapturePath $item.FullName
            $captures.Add([pscustomobject]@{
                Path = $item.FullName
                Bytes = $item.Length
                LastWriteUtcTicks = $item.LastWriteTimeUtc.Ticks
            })
        }
    }
}

[long]$totalBytes = 0
foreach ($capture in $captures) { $totalBytes += $capture.Bytes }
Write-Host ('Build directory: {0}' -f $buildRoot)
Write-Host ('Selected: {0:N0} raw captures, {1:N2} GiB' -f $captures.Count, ($totalBytes / 1GB))
Write-Host 'Preserving builds, reports, logs, previews, independent datasets, scene assets and Nsight traces.'

if (-not $Apply) {
    Write-Host 'DRY RUN: nothing deleted. Run again with -Apply to delete the selected captures.'
}
elseif ($WhatIfPreference) {
    Write-Host 'WHAT IF: nothing deleted.'
}
else {
    $active = @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -match '^(python|pythonw|scene_renderer_demo|EditorRenderingTest|ConeVoxelGIIntegrationTest|RayQueryIntegrationTest)\.exe$' -and
        ($_.ExecutablePath -like ($buildPrefix + '*') -or
         $_.CommandLine -match 'capture_hybrid|validate_hybrid|ground_truth_sweep')
    })
    if ($active.Count -gt 0) { throw 'Stop running render/capture tests before cleanup. No files were deleted.' }

    $manifest = Join-Path $buildRoot ('cleanup-captures-{0}.csv' -f (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
    $captures | Export-Csv -LiteralPath $manifest -NoTypeInformation -Encoding UTF8
    Write-Host ('Deletion manifest: {0}' -f $manifest)
    [long]$deletedBytes = 0
    [int]$deletedCount = 0
    try {
        if ($PSCmdlet.ShouldProcess($buildRoot, ('Delete {0} generated raw captures' -f $captures.Count))) {
            foreach ($capture in $captures) {
                Assert-CapturePath $capture.Path
                $current = Get-Item -LiteralPath $capture.Path -Force
                if ($current.Length -ne $capture.Bytes -or $current.LastWriteTimeUtc.Ticks -ne $capture.LastWriteUtcTicks) {
                    throw "Capture changed since inspection; stopped before deleting it: $($capture.Path)"
                }
                Remove-Item -LiteralPath $capture.Path -Force
                $deletedBytes += $capture.Bytes
                $deletedCount++
            }
        }
    }
    finally {
        Write-Host ('Deleted: {0:N0} files, {1:N2} GiB' -f $deletedCount, ($deletedBytes / 1GB))
    }
}
