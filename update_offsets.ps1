$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$destDir = Join-Path $repoRoot 'usermode\vendor\offsets'
$baseUrl = 'https://raw.githubusercontent.com/sezzyaep/CS2-OFFSETS/main'
$files = @('offsets.hpp', 'client_dll.hpp')

if (-not (Test-Path $destDir)) {
    New-Item -ItemType Directory -Path $destDir -Force | Out-Null
}

$changedFiles = @()
$failedFiles  = @()

foreach ($f in $files) {
    $dest = Join-Path $destDir $f
    $tmp  = "$dest.new"

    try {
        Invoke-WebRequest -Uri "$baseUrl/$f" -OutFile $tmp -UseBasicParsing | Out-Null
    } catch {
        Write-Host "[offsets] download failed for ${f}: $_"
        $failedFiles += $f
        if (Test-Path $tmp) { Remove-Item $tmp -Force }
        continue
    }

    $newHash = (Get-FileHash $tmp -Algorithm SHA256).Hash
    $oldHash = if (Test-Path $dest) { (Get-FileHash $dest -Algorithm SHA256).Hash } else { '' }

    if ($newHash -ne $oldHash) {
        Move-Item -Path $tmp -Destination $dest -Force
        $changedFiles += $f
    } else {
        Remove-Item $tmp -Force
    }
}

if ($changedFiles.Count -gt 0) {
    Write-Host ''
    foreach ($f in $changedFiles) { Write-Host "[offsets] $f updated" }
    Write-Host ''
    Write-Host '=== offsets have been updated, please rebuild before next run ==='
    Write-Host ''
} elseif ($failedFiles.Count -eq 0) {
    Write-Host '[offsets] no changes'
}
