# update_assets.ps1
# Обновляет manifest.json: sha256 + raw.githubusercontent.com/main URL для
# hero_hashes.dat, assets/*.png и gamestate_integration_dota2.cfg. Никаких
# GitHub Releases и версий — эти файлы уже лежат в main-ветке репозитория,
# приложение сверяет свои локальные копии напрямую с ней и перекачивает
# несовпадающие/недостающие файлы (см. checkForUpdates/downloadAndStageData/
# swapDataFiles и, отдельно для GSI-конфига, syncGsiConfig в finalapp/updater.cpp).
#
# Старые записи "assets/*", "hero_hashes.dat" и "gamestate_integration_dota2.cfg"
# в манифесте полностью заменяются текущим состоянием репозитория — иначе
# герои, пропавшие из assets/ (переименованные/убранные), остались бы в
# манифесте, а приложение продолжало бы их докачивать и хранить локально
# навсегда (см. cleanupObsoleteAssets в updater.cpp).
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$rawBaseApp       = 'https://raw.githubusercontent.com/yphilistine/dota_drafter/main/finalapp'
$rawBaseInstaller = 'https://raw.githubusercontent.com/yphilistine/dota_drafter/main/installer'

Write-Host '[1/3] Hashing hero_hashes.dat + assets/*.png...'
$files = [ordered]@{}
$files['hero_hashes.dat'] = 'finalapp\hero_hashes.dat'
Get-ChildItem 'finalapp\assets\*.png' | Sort-Object Name | ForEach-Object {
    $files["assets/$($_.Name)"] = $_.FullName
}

$assetEntries = [ordered]@{}
foreach ($key in $files.Keys) {
    $item = Get-Item -Path $files[$key]
    $hash = (Get-FileHash -Algorithm SHA256 -Path $item.FullName).Hash.ToLower()
    $assetEntries[$key] = [ordered]@{ url = "$rawBaseApp/$key"; sha256 = $hash; size = $item.Length }
}

Write-Host '[2/3] Hashing installer/gamestate_integration_dota2.cfg...'
$gsiKey  = 'gamestate_integration_dota2.cfg'
$gsiItem = Get-Item -Path "installer\$gsiKey"
$gsiHash = (Get-FileHash -Algorithm SHA256 -Path $gsiItem.FullName).Hash.ToLower()
$assetEntries[$gsiKey] = [ordered]@{ url = "$rawBaseInstaller/$gsiKey"; sha256 = $gsiHash; size = $gsiItem.Length }

Write-Host "[3/3] Updating manifest.json ($($assetEntries.Count) files)..."
$manifest = Get-Content 'manifest.json' -Raw | ConvertFrom-Json

# Сохраняем прочие записи (cbm/db — управляются pack_data.bat/GitHub Releases),
# полностью пересобираем только hero_hashes.dat + assets/* + GSI-конфиг.
$merged = [ordered]@{}
foreach ($p in $manifest.data.files.PSObject.Properties) {
    if ($p.Name -eq 'hero_hashes.dat' -or $p.Name -eq $gsiKey -or $p.Name.StartsWith('assets/')) { continue }
    $merged[$p.Name] = $p.Value
}
foreach ($key in $assetEntries.Keys) { $merged[$key] = $assetEntries[$key] }

$manifest.data.files = $merged
$manifest | ConvertTo-Json -Depth 10 | Set-Content 'manifest.json' -Encoding utf8

Write-Host "[DONE] hero_hashes.dat + $($assetEntries.Count - 2) assets + GSI config tracked in manifest.json"
