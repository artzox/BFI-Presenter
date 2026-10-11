<#
  Find-BFIPresenter.ps1  --  lists every game folder that has BFI Presenter installed.

  Looks in all Steam libraries (read from Steam's own library list), the usual Epic / GOG / Xbox
  folders, and any extra folders you pass with -Roots. For each folder it shows:
    - the DX add-on (BFIPresenter.addon64, or BFIPresenter.addon32 for 32-bit games) and its version
    - BFIPresenter.ini (a Vulkan-layer game has the ini but no add-on)
  It also shows the version of the Vulkan layer installed for your Windows user.

  Writes two files next to this script:
    BFIPresenter_locations.txt  -- one folder per line with the DX add-on (read by Update-BFIPresenter.ps1)
    BFIPresenter_locations.csv  -- everything found, with versions

  Run:  powershell -ExecutionPolicy Bypass -File .\Find-BFIPresenter.ps1
        powershell -ExecutionPolicy Bypass -File .\Find-BFIPresenter.ps1 -Roots "E:\Games","F:\Emulators"
  Changes nothing in the game folders.
#>
param(
    [string[]]$Roots = @(),
    [string]$OutDir = $PSScriptRoot
)

function Get-SteamLibraries {
    $libs = @()
    $steam = $null
    try { $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath } catch { }
    if (-not $steam) { $steam = 'C:\Program Files (x86)\Steam' }
    $vdf = [System.IO.Path]::Combine($steam, 'steamapps\libraryfolders.vdf')
    if (Test-Path -LiteralPath $vdf) {
        foreach ($line in Get-Content -LiteralPath $vdf) {
            if ($line -match '"path"\s+"([^"]+)"') { $libs += [System.IO.Path]::Combine(($matches[1] -replace '\\\\', '\'), 'steamapps\common') }
        }
    }
    $libs += ([System.IO.Path]::Combine($steam, 'steamapps\common'))
    return $libs
}

# Version of a BFI Presenter binary: the DX add-on carries "BFI Presenter v1.0.0" (v0.72 and later, as
# "v0.xx" before 1.0.0); the Vulkan layer "BFI Presenter (Vulkan layer) v1.0.0" (a bare "0.x.y" before 1.0.0).
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

$candidates = @(Get-SteamLibraries) + @(
    'C:\Program Files\Epic Games', 'D:\Epic Games', 'E:\Epic Games',
    'C:\GOG Games', 'D:\GOG Games', 'E:\GOG Games',
    'C:\XboxGames', 'D:\XboxGames', 'E:\XboxGames') + $Roots
$scan = @()
foreach ($c in $candidates) {
    if ($c -and (Test-Path -LiteralPath $c) -and ($scan -notcontains $c)) { $scan += $c }
}
if ($scan.Count -eq 0) { Write-Host 'No game folders found to scan. Pass them with -Roots "D:\Games".' -ForegroundColor Yellow; exit 1 }

Write-Host 'Scanning:' -ForegroundColor Cyan
$scan | ForEach-Object { Write-Host "  $_" }
Write-Host ''

$found = @{}
foreach ($root in $scan) {
    Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq 'BFIPresenter.addon64' -or $_.Name -eq 'BFIPresenter.addon32' -or $_.Name -eq 'BFIPresenter.ini' } |
        ForEach-Object {
            $dir = $_.DirectoryName
            if (-not $found.ContainsKey($dir)) { $found[$dir] = @{ Addon = $null; Bits = ''; Ini = $false } }
            if ($_.Name -eq 'BFIPresenter.addon64') { $found[$dir].Addon = $_.FullName; $found[$dir].Bits = '' }
            elseif ($_.Name -eq 'BFIPresenter.addon32') { if (-not $found[$dir].Addon) { $found[$dir].Addon = $_.FullName; $found[$dir].Bits = ', 32-bit' } }
            else { $found[$dir].Ini = $true }
        }
}

$rows = foreach ($dir in ($found.Keys | Sort-Object)) {
    $e = $found[$dir]
    if ($e.Addon) {
        $kind = if ($e.Ini) { 'DX add-on' + $e.Bits } else { 'DX add-on (no ini' + $e.Bits + ')' }
        $ver  = Get-BfiVersion $e.Addon
        $date = (Get-Item -LiteralPath $e.Addon).LastWriteTime.ToString('yyyy-MM-dd HH:mm')
    } else {
        $kind = 'Vulkan (ini only)'; $ver = ''; $date = ''
    }
    [PSCustomObject]@{ Type = $kind; Version = $ver; Date = $date; Folder = $dir }
}

if (-not $rows) {
    Write-Host 'No BFI Presenter installs found.' -ForegroundColor Yellow
} else {
    $rows | Format-Table -AutoSize | Out-String -Width 400 | Write-Host
    $dx = @($rows | Where-Object { $_.Type -like 'DX*' })
    $vk = @($rows | Where-Object { $_.Type -like 'Vulkan*' })
    $latest = ($dx | Where-Object { $_.Version -match '^\d+\.\d+(\.\d+)?$' } |
               Sort-Object { [version]$_.Version } -Descending | Select-Object -First 1).Version
    Write-Host ("DX add-on in {0} folder(s), Vulkan-only ini in {1} folder(s)." -f $dx.Count, $vk.Count) -ForegroundColor Cyan
    if ($latest) {
        $old = @($dx | Where-Object { $_.Version -ne $latest })
        if ($old.Count) { Write-Host ("{0} folder(s) are older than v{1}." -f $old.Count, $latest) -ForegroundColor Yellow }
    }
    $listFile = [System.IO.Path]::Combine($OutDir, 'BFIPresenter_locations.txt')
    $csvFile  = [System.IO.Path]::Combine($OutDir, 'BFIPresenter_locations.csv')
    $dx | ForEach-Object { $_.Folder } | Set-Content -LiteralPath $listFile -Encoding UTF8
    $rows | Export-Csv -LiteralPath $csvFile -NoTypeInformation -Encoding UTF8
    Write-Host "Saved: $listFile"
    Write-Host "Saved: $csvFile"
}

$layerDir = [System.IO.Path]::Combine($env:LOCALAPPDATA, 'BFIPresenter', 'Vulkan')
$layerDll = [System.IO.Path]::Combine($layerDir, 'BFIPresenterVK.dll')
Write-Host ''
if (Test-Path -LiteralPath $layerDll) {
    Write-Host ("Vulkan layer v{0} installed in {1}" -f (Get-BfiVersion $layerDll -Vulkan), $layerDir) -ForegroundColor Cyan
} else {
    Write-Host 'Vulkan layer not installed for this user (install_vulkan_layer.bat).'
}
