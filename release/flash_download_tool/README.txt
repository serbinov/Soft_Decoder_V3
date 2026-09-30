================================================================
 Flash Download Tool - файлы и смещения
 Прошивка ADDITIPUS (ESP32-S3, Flash 4 МБ)
================================================================

Файлы создаются скриптом: firmware\build_flash_tool_files.bat
Этот README генерируется из partitions.csv (tools\gen_flash_readme.ps1).

Загрузите в Flash Download Tool следующие файлы (в указанном порядке):

  Файл                   Смещение
  ---------------------  --------
  bootloader.bin         0x0000
  partitions.bin         0x8000
  ota_data_initial.bin   0x01a000
  firmware.bin           0x020000   (слот ota_0)

Настройки в Flash Download Tool:

  ChipType   : ESP32-S3
  FlashSize  : 4 MB
  SPI Speed  : 80 MHz
  SPI Mode   : DIO
  Baud Rate  : 921600 (или 460800)

Таблица разделов (partitions.csv):

  Имя        Offset     Размер    Тип
  ---------  ---------  --------  --------
  nvs        0x009000      64 KiB  data/nvs
  phy_init   0x019000       4 KiB  data/phy
  otadata    0x01a000       8 KiB  data/ota
  ota_0      0x020000    1920 KiB  app/ota_0
  ota_1      0x200000    1920 KiB  app/ota_1
  coredump   0x3e0000     128 KiB  data/coredump
  Сумма разделов:                           4044 KiB
  Резерв (bootloader + таблица + выравнивание):    52 KiB
  Всего flash:                             4096 KiB

Примечания:

  * firmware.bin пишется в слот ota_0; его смещение = offset слота ota_0.
  * Внутреннего раздела userdata больше нет: звуки хранятся только на
    внешнем W25Q128. При отсутствии/сбое внешней памяти устройство
    продолжает работать (мотор, DCC, веб), но без звуков.
  * Перед записью на плату с ПРЕЖНЕЙ разметкой обязательно выполните ERASE
    (полная очистка flash): таблица разделов изменилась, и старая область
    userdata (0x1f0000) теперь попадает внутрь нового слота ota_1.
  * phy_init (0x019000) не загружается: RF-калибровка генерируется прошивкой.
  * ota_data_initial.bin сбрасывает выбор загрузки на ota_0.
  * После прошивки нажмите START; по завершении плата перезагрузится.

