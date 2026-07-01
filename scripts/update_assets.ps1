# update_assets.ps1
# Обновляет manifest.json: sha256 + raw.githubusercontent.com/main URL для
# hero_hashes.dat и assets/*.png. Никаких GitHub Releases и версий — эти файлы
# уже лежат в main-ветке репозитория, приложение сверяет свои локальные копии
# напрямую с ней и перекачивает несовпадающие/недостающие файлы (см.
# checkForUpdates/downloadAndStageData/swapDataFiles в finalapp/updater.cpp).
#
# Старые записи "assets/*" и "hero_hashes.dat" в манифесте полностью
# заменяются текущим состоянием finalapp/assets — иначе герои, пропавшие из
# assets/ (переименованные/убранные), остались бы в манифесте, а приложение
# продолжало бы их докачивать и хранить локально навсегда
# (см. cleanupObsoleteAssets в updater.cpp).
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$rawBase = 'https://raw.githubusercontent.com/yphilistine/dota_drafter/main/finalapp'

Write-Host '[1/2] Hashing hero_hashes.dat + assets/*.png...'
$files = [ordered]@{}
$files['hero_hashes.dat'] = 'finalapp\hero_hashes.dat'
Get-ChildItem 'finalapp\assets\*.png' | Sort-Object Name | ForEach-Object {
    $files["assets/$($_.Name)"] = $_.FullName
}

$assetEntries = [ordered]@{}
foreach ($key in $files.Keys) {
    $hash = (Get-FileHash -Algorithm SHA256 -Path $files[$key]).Hash.ToLower()
    $assetEntries[$key] = [ordered]@{ url = "$rawBase/$key"; sha256 = $hash }
}

Write-Host "[2/2] Updating manifest.json ($($assetEntries.Count) files)..."
$manifest = Get-Content 'manifest.json' -Raw | ConvertFrom-Json

# Сохраняем прочие записи (cbm/db — управляются pack_data.bat/GitHub Releases),
# полностью пересобираем только hero_hashes.dat + assets/*.
$merged = [ordered]@{}
foreach ($p in $manifest.data.files.PSObject.Properties) {
    if ($p.Name -eq 'hero_hashes.dat' -or $p.Name.StartsWith('assets/')) { continue }
    $merged[$p.Name] = $p.Value
}
foreach ($key in $assetEntries.Keys) { $merged[$key] = $assetEntries[$key] }

$manifest.data.files = $merged
$manifest | ConvertTo-Json -Depth 10 | Set-Content 'manifest.json' -Encoding utf8

Write-Host "[DONE] hero_hashes.dat + $($assetEntries.Count - 1) assets tracked in manifest.json"
