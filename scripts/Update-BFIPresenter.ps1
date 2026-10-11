<#
  Update-BFIPresenter.ps1  --  copies a new BFI Presenter build into every game folder that has it.

  Reads BFIPresenter_locations.txt (made by Find-BFIPresenter.ps1) and replaces BFIPresenter.addon64 / .addon32
  in each listed folder. If the source also has the Vulkan layer, the installed layer is updated too.
  BFIPresenter.ini files are never touched (your per-game settings stay as they are).

  Every file it replaces is kept, and every change is logged, next to this script:
    BFIPresenter_update_log.csv  -- one line per replaced file: time, folder, file, old and new version
    BFIPresenter_backups\        -- the replaced files, one copy per version (kept until you delete them)
  If a new version misbehaves, -Revert puts back what the last update replaced.

  -Source  folder with the new build: the extracted BFIPresenter-DX-vX.zip and/or BFIPresenter-Vulkan-vX.zip
           (the files directly in it, or in its BFIPresenter / BFIPresenterVK subfolders).
           Default: the folder this script is in.
  -DryRun  only show what would change (also with -Revert).
  -Revert  undo the last update: copy back the files it replaced, from BFIPresenter_backups.
  -Backup  also keep the old add-on as BFIPresenter.addon64.bak (or .addon32.bak) in each game folder.

  Run:  Update-BFIPresenter.bat -DryRun
        Update-BFIPresenter.bat -Source "D:\BFIPresenter_dev\latest"
        Update-BFIPresenter.bat -Revert
  Close the games first: a running game keeps its add-on locked.
#>
param(
    [string]$Source = $PSScriptRoot,
    [string]$ListFile = ([System.IO.Path]::Combine($PSScriptRoot, 'BFIPresenter_locations.txt')),
    [switch]$DryRun,
    [switch]$Revert,
    [switch]$Backup
)

$LogFile   = [System.IO.Path]::Combine($PSScriptRoot, 'BFIPresenter_update_log.csv')
$BackupDir = [System.IO.Path]::Combine($PSScriptRoot, 'BFIPresenter_backups')
$RunId     = (Get-Date).ToString('yyyy-MM-dd HH:mm:ss')

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
function Get-FileVersion([string]$file) {
    if ($file -like '*.json') { return '' }
    if ($file -like '*BFIPresenterVK*') { return Get-BfiVersion $file -Vulkan }
    return Get-BfiVersion $file
}

# A copy of a file about to be replaced, once per distinct build:
# BFIPresenter_backups\<file name>\<version>_<first 12 hash digits>\<file name>
function Save-Backup([string]$file, [string]$version, [string]$hash) {
    $name = [System.IO.Path]::GetFileName($file)
    $tag  = ($version -replace '[^0-9A-Za-z.\-]', '_') + '_' + $hash.Substring(0, 12)
    $dir  = [System.IO.Path]::Combine($BackupDir, $name, $tag)
    $dst  = [System.IO.Path]::Combine($dir, $name)
    if (-not (Test-Path -LiteralPath $dst)) {
        New-Item -ItemType Directory -Path $dir -Force -ErrorAction Stop | Out-Null
        Copy-Item -LiteralPath $file -Destination $dst -Force -ErrorAction Stop
    }
    return $dst
}

function Write-Log([string]$action, [string]$folder, [string]$file, [string]$oldVer, [string]$newVer,
                   [string]$oldHash, [string]$newHash, [string]$saved) {
    $row = [PSCustomObject]@{
        Run = $RunId; Action = $action; Folder = $folder; File = $file
        OldVersion = $oldVer; NewVersion = $newVer; OldSha256 = $oldHash; NewSha256 = $newHash; Backup = $saved
    }
    $row | Export-Csv -LiteralPath $LogFile -NoTypeInformation -Append -Encoding UTF8
}

$updated = 0; $current = 0; $failed = 0; $missing = 0

# Copy $src over $dst: keep the old file in the backup store, log the change.
function Update-One([string]$src, [string]$dst, [string]$label) {
    $newHash = Get-Hash $src
    $exists  = Test-Path -LiteralPath $dst
    if ($exists -and (Get-Hash $dst) -eq $newHash) { Write-Host "  current   $label"; $script:current++; return }
    $newVer = Get-FileVersion $src
    $oldVer = if ($exists) { Get-FileVersion $dst } else { '' }
    $change = if ($exists -and $newVer) { "{0} -> {1}  " -f (Format-Ver $oldVer), (Format-Ver $newVer) } else { '' }
    if ($DryRun) { Write-Host ("  would update  {0}{1}" -f $change, $label) -ForegroundColor Green; $script:updated++; return }
    try {
        $oldHash = ''; $saved = ''
        if ($exists) {
            $oldHash = Get-Hash $dst
            $saved = Save-Backup $dst $oldVer $oldHash
            if ($Backup -and $dst -like '*.addon*') { Copy-Item -LiteralPath $dst -Destination ($dst + '.bak') -Force -ErrorAction Stop }
        }
        Copy-Item -LiteralPath $src -Destination $dst -Force -ErrorAction Stop
        Write-Log 'updated' (Split-Path $dst) ([System.IO.Path]::GetFileName($dst)) $oldVer $newVer $oldHash $newHash $saved
        Write-Host ("  updated   {0}{1}" -f $change, $label) -ForegroundColor Green
        $script:updated++
    } catch {
        Write-Host "  FAILED    $label  ($($_.Exception.Message) -- is the game running?)" -ForegroundColor Red
        $script:failed++
    }
}

# ---------------------------------------------------------------- -Revert
if ($Revert) {
    if (-not (Test-Path -LiteralPath $LogFile)) { Write-Host "$LogFile not found: nothing to revert." -ForegroundColor Yellow; exit 1 }
    $log = @(Import-Csv -LiteralPath $LogFile)
    $lastRun = ($log | Where-Object { $_.Action -eq 'updated' } | Select-Object -Last 1).Run
    if (-not $lastRun) { Write-Host 'The log has no update to revert.' -ForegroundColor Yellow; exit 1 }
    $rows = @($log | Where-Object { $_.Run -eq $lastRun -and $_.Action -eq 'updated' })
    Write-Host "Reverting the update of $lastRun ($($rows.Count) file(s))" -ForegroundColor Cyan
    if ($DryRun) { Write-Host 'Dry run: nothing will be changed.' -ForegroundColor Yellow }
    foreach ($r in $rows) {
        $dst = [System.IO.Path]::Combine($r.Folder, $r.File)
        $label = "$($r.Folder)  ($($r.File))"
        if (-not $r.Backup -or -not (Test-Path -LiteralPath $r.Backup)) {
            Write-Host "  no backup $label  (it was newly added, or the backup was deleted)" -ForegroundColor DarkYellow; $missing++; continue
        }
        if ((Test-Path -LiteralPath $dst) -and (Get-Hash $dst) -ne $r.NewSha256) {
            Write-Host "  skipped   $label  (changed since that update -- left as it is)" -ForegroundColor DarkYellow; $missing++; continue
        }
        if ($DryRun) { Write-Host ("  would revert {0} -> {1}  {2}" -f (Format-Ver $r.NewVersion), (Format-Ver $r.OldVersion), $label) -ForegroundColor Green; $updated++; continue }
        try {
            Copy-Item -LiteralPath $r.Backup -Destination $dst -Force -ErrorAction Stop
            Write-Log 'reverted' $r.Folder $r.File $r.NewVersion $r.OldVersion $r.NewSha256 $r.OldSha256 $r.Backup
            Write-Host ("  reverted  {0} -> {1}  {2}" -f (Format-Ver $r.NewVersion), (Format-Ver $r.OldVersion), $label) -ForegroundColor Green
            $updated++
        } catch {
            Write-Host "  FAILED    $label  ($($_.Exception.Message) -- is the game running?)" -ForegroundColor Red
            $failed++
        }
    }
    # The reverted run is marked so the next -Revert goes one update further back.
    if (-not $DryRun -and $updated) {
        $all = @(Import-Csv -LiteralPath $LogFile)
        foreach ($row in $all) { if ($row.Run -eq $lastRun -and $row.Action -eq 'updated') { $row.Action = 'updated (reverted)' } }
        $all | Export-Csv -LiteralPath $LogFile -NoTypeInformation -Encoding UTF8
    }
    Write-Host ''
    $verb = if ($DryRun) { 'would be reverted' } else { 'reverted' }
    Write-Host ("{0} {1}, {2} failed, {3} skipped." -f $updated, $verb, $failed, $missing) -ForegroundColor Cyan
    exit 0
}

# ---------------------------------------------------------------- update
$newAddon   = Find-InSource 'BFIPresenter.addon64' 'BFIPresenter'
$newAddon32 = Find-InSource 'BFIPresenter.addon32' 'BFIPresenter'
$newVk64    = Find-InSource 'BFIPresenterVK.dll' 'BFIPresenterVK'
if (-not $newAddon -and -not $newAddon32 -and -not $newVk64) {
    Write-Host "No BFIPresenter.addon64 / .addon32 or BFIPresenterVK.dll found in $Source (or its BFIPresenter / BFIPresenterVK subfolders)." -ForegroundColor Red
    exit 1
}
if ($DryRun) { Write-Host 'Dry run: nothing will be changed.' -ForegroundColor Yellow }

# ---- DX add-on (64- and 32-bit) in every listed game folder
if ($newAddon -or $newAddon32) {
    if (-not (Test-Path -LiteralPath $ListFile)) {
        Write-Host "$ListFile not found -- run Find-BFIPresenter.ps1 first." -ForegroundColor Red
        exit 1
    }
    $kinds = @()
    if ($newAddon)   { $kinds += ,@('BFIPresenter.addon64', $newAddon) }
    if ($newAddon32) { $kinds += ,@('BFIPresenter.addon32', $newAddon32) }
    foreach ($k in $kinds) { Write-Host ("DX add-on: v{0} from {1}" -f (Get-BfiVersion $k[1]), $k[1]) -ForegroundColor Cyan }
    $folders = Get-Content -LiteralPath $ListFile | Where-Object { $_.Trim() -ne '' }
    foreach ($dir in $folders) {
        $any = $false
        foreach ($k in $kinds) {
            $dst = [System.IO.Path]::Combine($dir, $k[0])
            if (-not (Test-Path -LiteralPath $dst)) { continue }
            $any = $true
            Update-One $k[1] $dst "$dir  ($($k[0]))"
        }
        if (-not $any) {
            Write-Host "  missing   $dir  (no add-on there any more -- re-run Find-BFIPresenter.ps1)" -ForegroundColor DarkYellow
            $missing++
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
            if (-not (Test-Path -LiteralPath $src)) { continue }
            Update-One $src ([System.IO.Path]::Combine($layerDir, $name)) $name
        }
    }
}

Write-Host ''
$verb = if ($DryRun) { 'would be updated' } else { 'updated' }
Write-Host ("{0} {1}, {2} already current, {3} failed, {4} missing." -f $updated, $verb, $current, $failed, $missing) -ForegroundColor Cyan
if (-not $DryRun -and $updated) {
    Write-Host "Logged in $LogFile; the replaced files are in $BackupDir."
    Write-Host 'If the new version misbehaves:  Update-BFIPresenter.bat -Revert'
}
Write-Host 'BFIPresenter.ini files were not changed. New settings use their defaults until you save them from the overlay.'
