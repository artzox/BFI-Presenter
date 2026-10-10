<#
  Update-BFIPresenter.ps1  --  copies a new BFI Presenter build into every game folder that has it.

  Reads BFIPresenter_locations.txt (made by Find-BFIPresenter.ps1) and replaces BFIPresenter.addon64
  in each listed folder. If the source also has the Vulkan layer, the installed layer is updated too.
  BFIPresenter.ini files are never touched (your per-game settings stay as they are).

  -Source  folder with the new build: the extracted BFIPresenter-DX-vX.zip and/or BFIPresenter-Vulkan-vX.zip
           (the files directly in it, or in its BFIPresenter / BFIPresenterVK subfolders).
           Default: the folder this script is in.
  -DryRun  only show what would change.
  -Backup  keep the old add-on as BFIPresenter.addon64.bak in each folder.

  Run:  powershell -ExecutionPolicy Bypass -File .\Update-BFIPresenter.ps1 -DryRun
        powershell -ExecutionPolicy Bypass -File .\Update-BFIPresenter.ps1 -Source "D:\BFIPresenter_dev\latest"
  Close the games first: a running game keeps its add-on locked.
#>
param(
    [string]$Source = $PSScriptRoot,
    [string]$ListFile = ([System.IO.Path]::Combine($PSScriptRoot, 'BFIPresenter_locations.txt')),
    [switch]$DryRun,
    [switch]$Backup
)

function Get-BfiVersion([string]$file, [switch]$Vulkan) {
    try {
        $text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($file))
        if ($Vulkan) {
            if ($text -match 'BFI Presenter \(Vulkan layer\) v(\d+\.\d+\.\d+)') { return $matches[1] }   # 1.0.0 and later
            if ($text -match '\x00(0\.\d+\.\d+)\x00') { return $matches[1] }                          # 0.7-0.8.x
        } elseif ($text -match 'BFI Presenter v(\d+\.\d+(?:\.\d+)?)') {
            return $matches[1]
        }
        return 'older than 0.72'
    } catch { return '?' }
}
function Format-Ver([string]$v) { if ($v -match '^\d') { return 'v' + $v } return $v }
function Find-InSource([string]$name, [string]$sub) {
    foreach ($p in @(([System.IO.Path]::Combine($Source, $name)), ([System.IO.Path]::Combine($Source, $sub, $name)))) {
        if (Test-Path -LiteralPath $p) { return $p }
    }
    return $null
}
function Get-Hash([string]$file) { (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash }

$newAddon = Find-InSource 'BFIPresenter.addon64' 'BFIPresenter'
$newVk64  = Find-InSource 'BFIPresenterVK.dll' 'BFIPresenterVK'
if (-not $newAddon -and -not $newVk64) {
    Write-Host "No BFIPresenter.addon64 or BFIPresenterVK.dll found in $Source (or its BFIPresenter / BFIPresenterVK subfolders)." -ForegroundColor Red
    exit 1
}
if ($DryRun) { Write-Host 'Dry run: nothing will be changed.' -ForegroundColor Yellow }

$updated = 0; $current = 0; $failed = 0; $missing = 0

# ---- DX add-on in every listed game folder
if ($newAddon) {
    if (-not (Test-Path -LiteralPath $ListFile)) {
        Write-Host "$ListFile not found -- run Find-BFIPresenter.ps1 first." -ForegroundColor Red
        exit 1
    }
    $newVer  = Get-BfiVersion $newAddon
    $newHash = Get-Hash $newAddon
    Write-Host ("DX add-on: v{0} from {1}" -f $newVer, $newAddon) -ForegroundColor Cyan
    $folders = Get-Content -LiteralPath $ListFile | Where-Object { $_.Trim() -ne '' }
    foreach ($dir in $folders) {
        $dst = [System.IO.Path]::Combine($dir, 'BFIPresenter.addon64')
        if (-not (Test-Path -LiteralPath $dst)) {
            Write-Host "  missing   $dir  (no add-on there any more -- re-run Find-BFIPresenter.ps1)" -ForegroundColor DarkYellow
            $missing++; continue
        }
        if ((Get-Hash $dst) -eq $newHash) { Write-Host "  current   $dir"; $current++; continue }
        $oldVer = Get-BfiVersion $dst
        if ($DryRun) { Write-Host ("  would update {0} -> {1}  {2}" -f (Format-Ver $oldVer), (Format-Ver $newVer), $dir) -ForegroundColor Green; $updated++; continue }
        try {
            if ($Backup) { Copy-Item -LiteralPath $dst -Destination ($dst + '.bak') -Force -ErrorAction Stop }
            Copy-Item -LiteralPath $newAddon -Destination $dst -Force -ErrorAction Stop
            Write-Host ("  updated   {0} -> {1}  {2}" -f (Format-Ver $oldVer), (Format-Ver $newVer), $dir) -ForegroundColor Green
            $updated++
        } catch {
            Write-Host "  FAILED    $dir  ($($_.Exception.Message) -- is the game running?)" -ForegroundColor Red
            $failed++
        }
    }
}

# ---- Vulkan layer (installed once per Windows user by install_vulkan_layer.bat)
if ($newVk64) {
    $layerDir = [System.IO.Path]::Combine($env:LOCALAPPDATA, 'BFIPresenter', 'Vulkan')
    Write-Host ''
    if (-not (Test-Path -LiteralPath $layerDir)) {
        Write-Host 'Vulkan layer not installed for this user: run install_vulkan_layer.bat from the Vulkan package once.' -ForegroundColor DarkYellow
    } else {
        Write-Host ("Vulkan layer: v{0} from {1}" -f (Get-BfiVersion $newVk64 -Vulkan), (Split-Path $newVk64)) -ForegroundColor Cyan
        $srcDir = Split-Path $newVk64
        foreach ($name in 'BFIPresenterVK.dll', 'BFIPresenterVK32.dll', 'VkLayer_BFI_presenter.json', 'VkLayer_BFI_presenter32.json') {
            $src = [System.IO.Path]::Combine($srcDir, $name)
            $dst = [System.IO.Path]::Combine($layerDir, $name)
            if (-not (Test-Path -LiteralPath $src)) { continue }
            if ((Test-Path -LiteralPath $dst) -and ((Get-Hash $dst) -eq (Get-Hash $src))) { Write-Host "  current   $name"; $current++; continue }
            if ($DryRun) { Write-Host "  would update  $name" -ForegroundColor Green; $updated++; continue }
            try {
                Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop
                Write-Host "  updated   $name" -ForegroundColor Green
                $updated++
            } catch {
                Write-Host "  FAILED    $name  ($($_.Exception.Message) -- close any running Vulkan game)" -ForegroundColor Red
                $failed++
            }
        }
    }
}

Write-Host ''
$verb = if ($DryRun) { 'would be updated' } else { 'updated' }
Write-Host ("{0} {1}, {2} already current, {3} failed, {4} missing." -f $updated, $verb, $current, $failed, $missing) -ForegroundColor Cyan
Write-Host 'BFIPresenter.ini files were not changed. New settings use their defaults until you save them from the overlay.'
