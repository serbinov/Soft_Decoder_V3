# Generates release/flash_download_tool/README.txt from partitions.csv so the
# documented offsets/sizes always match the real partition table.
param(
    [string]$Csv = (Join-Path (Split-Path -Parent $PSScriptRoot) "partitions.csv"),
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Out)) {
    $root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
    $Out = Join-Path $root "release\flash_download_tool\README.txt"
}

function To-KiB([string]$hex) {
    try { return [int64]([Convert]::ToInt64($hex.Trim(), 16) / 1024) } catch { return 0 }
}

$rows = @()
Get-Content -LiteralPath $Csv | ForEach-Object {
    $line = $_.Trim()
    if ($line -eq "" -or $line.StartsWith("#")) { return }
    $p = $line.Split(",") | ForEach-Object { $_.Trim() }
    if ($p.Count -lt 5) { return }
    $rows += [pscustomobject]@{
        Name    = $p[0]
        Type    = $p[1]
        Sub     = $p[2]
        Offset  = $p[3]
        Size    = $p[4]
        SizeKiB = (To-KiB $p[4])
    }
}

$totalKiB = ($rows | Measure-Object -Property SizeKiB -Sum).Sum
$fw = $rows | Where-Object { $_.Sub -eq "ota_0" } | Select-Object -First 1
$fwOffset = if ($fw) { $fw.Offset } else { "0x20000" }
$phy = $rows | Where-Object { $_.Sub -eq "phy" } | Select-Object -First 1
$phyOffset = if ($phy) { $phy.Offset } else { "0x019000" }
$ota = $rows | Where-Object { $_.Sub -eq "ota" } | Select-Object -First 1
$otaOffset = if ($ota) { $ota.Offset } else { "0x01a000" }

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("================================================================")
[void]$sb.AppendLine(" Flash Download Tool - файлы и смещения")
[void]$sb.AppendLine(" Прошивка ADDITIPUS (ESP32-S3, Flash 4 МБ)")
[void]$sb.AppendLine("================================================================")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Файлы создаются скриптом: firmware\build_flash_tool_files.bat")
[void]$sb.AppendLine("Этот README генерируется из partitions.csv (tools\gen_flash_readme.ps1).")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Загрузите в Flash Download Tool следующие файлы (в указанном порядке):")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("  Файл                   Смещение")
[void]$sb.AppendLine("  ---------------------  --------")
[void]$sb.AppendLine("  bootloader.bin         0x0000")
[void]$sb.AppendLine("  partitions.bin         0x8000")
[void]$sb.AppendLine(("  ota_data_initial.bin   {0}" -f $otaOffset))
[void]$sb.AppendLine(("  firmware.bin           {0}   (слот ota_0)" -f $fwOffset))
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Настройки в Flash Download Tool:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("  ChipType   : ESP32-S3")
[void]$sb.AppendLine("  FlashSize  : 4 MB")
[void]$sb.AppendLine("  SPI Speed  : 80 MHz")
[void]$sb.AppendLine("  SPI Mode   : DIO")
[void]$sb.AppendLine("  Baud Rate  : 921600 (или 460800)")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Таблица разделов (partitions.csv):")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("  Имя        Offset     Размер    Тип")
[void]$sb.AppendLine("  ---------  ---------  --------  --------")
foreach ($r in $rows) {
    [void]$sb.AppendLine(("  {0,-9}  {1,-9}  {2,5} KiB  {3}/{4}" -f $r.Name, $r.Offset, $r.SizeKiB, $r.Type, $r.Sub))
}
[void]$sb.AppendLine(("  Сумма разделов:                          {0,5} KiB" -f $totalKiB))
[void]$sb.AppendLine(("  Резерв (bootloader + таблица + выравнивание): {0,5} KiB" -f (4096 - $totalKiB)))
[void]$sb.AppendLine("  Всего flash:                             4096 KiB")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("Примечания:")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("  * firmware.bin пишется в слот ota_0; его смещение = offset слота ota_0.")
[void]$sb.AppendLine("  * Внутреннего раздела userdata больше нет: звуки хранятся только на")
[void]$sb.AppendLine("    внешнем W25Q128. При отсутствии/сбое внешней памяти устройство")
[void]$sb.AppendLine("    продолжает работать (мотор, DCC, веб), но без звуков.")
[void]$sb.AppendLine("  * Перед записью на плату с ПРЕЖНЕЙ разметкой обязательно выполните ERASE")
[void]$sb.AppendLine("    (полная очистка flash): таблица разделов изменилась, и старая область")
[void]$sb.AppendLine("    userdata (0x1f0000) теперь перекрывается новыми OTA-слотами (ota_0/ota_1).")
[void]$sb.AppendLine(("  * phy_init ({0}) не загружается: RF-калибровка генерируется прошивкой." -f $phyOffset))
[void]$sb.AppendLine("  * ota_data_initial.bin сбрасывает выбор загрузки на ota_0.")
[void]$sb.AppendLine("  * После прошивки нажмите START; по завершении плата перезагрузится.")
[void]$sb.AppendLine("")

$dir = Split-Path -Parent $Out
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
[System.IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($true)))
Write-Host "[OK] README written: $Out"
