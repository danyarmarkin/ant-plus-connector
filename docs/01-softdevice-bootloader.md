# 01 — SoftDevice S340 и загрузчик

## Зачем менять SoftDevice

nRF52840 сам по себе не знает протокол ANT: радиостек — это закрытый бинарник Nordic/Garmin
(**SoftDevice**), который живёт в начале флеша. nice!nano продаётся с Adafruit UF2 bootloader и
**S140** — это только BLE. ANT есть в «комбинированных» стеках Garmin:

| SoftDevice | Протоколы | Чип |
|---|---|---|
| S140 | BLE | nRF52840 (стоит на nice!nano из коробки) |
| **S340** | **ANT + BLE** | **nRF52840** |
| S212 / S332 | ANT / ANT + BLE | nRF52832 и др. |

Реализовать ANT «вручную» на голом радио не получится: протокол закрыт, а сети ANT+ требуют ключа.

## Где взять S340

Раньше стеки ANT выдавались после регистрации ANT+ Adopter на thisisant.com (около суток ожидания).
Теперь Garmin раздаёт их **напрямую**: <https://developer.garmin.com/ant-program/nrf5-sdk/>.

```bash
scripts/get_softdevice.sh          # спросит согласие с лицензией, скачает ~10 МБ, распакует S340 7.0.1 в garmin/
```

В архиве `garmin/s340_nrf52_7.0.1/`:

- `ANT_s340_nrf52_7.0.1.hex` — сам стек (MBR + SoftDevice, `0x00000–0x31000`);
- `ANT_s340_nrf52_7.0.1.API/include/` — заголовки (`ant_interface.h`, `ant_parameters.h`, `nrf_sdm.h`, …);
- в `nrf_sdm.h` закомментирован **evaluation-ключ** `ANT_LICENSE_KEY` — им стек «разблокируется»
  при `sd_softdevice_enable()`. Только для некоммерческого использования.

Лицензия: free для evaluation/некоммерческого применения; коммерческий ключ — $0.08 за устройство
(минимум $800 за полгода), форма запроса на странице Garmin.

## Раскладка флеша

```
0x00000  MBR
0x01000  SoftDevice S340 7.0.1            (SD_FLASH_SIZE = 0x30000)
0x31000  приложение (скетч Arduino)        <- у S140 6.1.1 было 0x26000
0xED000  InternalFS (LittleFS, сохранённый выбор датчика)
0xF4000  Adafruit UF2 bootloader
0xFD800  настройки загрузчика
```

Из-за сдвига начала приложения **скетчи, собранные под S140, на плате с S340 не запустятся** —
собирайте под плату «nice!nano (S340 ANT+BLE)» (см. [02](02-arduino-board.md)).

## Прошивка без программатора

Adafruit bootloader поддерживает обновление «загрузчик + SoftDevice» по USB (serial DFU, протокол
Nordic SDK 11). Код загрузчика рассчитан на то, что новый стек больше старого: образ принимается со
смещением, а перенос на место выполняет MBR блоками с проверкой, устойчивой к пропаданию питания.

Официальный Adafruit bootloader уже умеет собираться с S340 (`lib/softdevice/s340_nrf52_7.0.1/readme.md`).
Скрипт делает всё сам:

```bash
scripts/flash_bootloader.sh                # собрать + прошить (спросит подтверждение)
scripts/flash_bootloader.sh --build-only   # только собрать
```

Что происходит внутри:

1. клонирует `adafruit/Adafruit_nRF52_Bootloader` (проверено на `0.11.0`, коммит `c67f0bc`) в `build/bootloader/`;
2. кладёт `.hex` и заголовки S340 в `lib/softdevice/s340_nrf52_7.0.1/`;
3. `make BOARD=nice_nano SD_NAME=s340 SD_VERSION=7.0.1 ANT_LICENSE_KEY=<ключ> all`
   (версия передаётся явно через `_VER3` — `Makefile` разбирает её GNU-`sed`-синтаксисом, который
   не работает в macOS);
4. ставит `adafruit-nrfutil` в venv и запускает
   `adafruit-nrfutil dfu serial --package nice_nano_bootloader-*_s340_7.0.1.zip -p <порт> -b 115200 --singlebank`.

Перед запуском плата должна быть в режиме загрузчика: двойное нажатие reset (или замыкание RST–GND
дважды) — появится диск `NICENANO`. Порт — `/dev/cu.usbmodem*` (macOS) / `/dev/ttyACM*` (Linux).

После прошивки `NICENANO/INFO_UF2.TXT`:

```
UF2 Bootloader 0.11.0
Model: nice!nano
Board-ID: nRF52840-nicenano
SoftDevice: S340 7.0.1
```

Обновление занимает ~25 секунд. **Не отключайте кабель**: если оборвать процесс в момент переноса
SoftDevice, без SWD-программатора плату будет нечем восстановить.

> Совет: перед прошивкой сохраните `NICENANO/CURRENT.UF2` — это дамп текущего приложения.

## Откат на S140

Тем же способом: соберите загрузчик с `SD_NAME=s140 SD_VERSION=6.1.1` или возьмите готовый
`nice_nano_bootloader-*_s140_6.1.1.zip` из [релизов Adafruit](https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases)
и прошейте `adafruit-nrfutil dfu serial ... --singlebank`.
