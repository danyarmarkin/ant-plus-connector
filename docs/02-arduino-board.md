# 02 — Плата в Arduino IDE

Прошивка собирается ядром **Adafruit nRF52** (Boards Manager, URL
`https://adafruit.github.io/arduino-board-index/package_adafruit_index.json`, проверено на 1.7.0).
В ядре нет ни S340, ни nice!nano, поэтому `arduino_s340/install.sh` добавляет плату
**«nice!nano (S340 ANT+BLE)»** прямо в установленное ядро:

```bash
arduino_s340/install.sh
```

| Файл в ядре | Откуда | Зачем |
|---|---|---|
| `cores/nRF5/nordic/softdevice/s340_nrf52_7.0.1_API/` | `garmin/s340_nrf52_7.0.1/…API` | заголовки S340: BLE + `ant_interface.h` |
| `cores/nRF5/linker/nrf52840_s340_v7.ld` | `arduino_s340/` | FLASH с `0x31000`, RAM с `0x20008000` |
| `variants/nicenano_s340/` | `arduino_s340/variant_nicenano_s340/` | вариант на базе Feather nRF52840 |
| `boards.local.txt` | `arduino_s340/boards.local.txt` | описание платы + ключ лицензии ANT |

Ядро Adafruit уже поддерживает ANT «изнутри»: если определён `ANT_LICENSE_KEY`, `Bluefruit.begin()`
вызывает `sd_softdevice_enable(..., ANT_LICENSE_KEY)` и умеет пробрасывать события SoftDevice
в пользовательский семафор (`Bluefruit.setMultiprotocolSemaphore`). Плата лишь передаёт
`-DANT_LICENSE_KEY="…"` и пути к заголовкам S340.

## Ключ лицензии

`boards.local.txt` в репозитории содержит плейсхолдер `@ANT_LICENSE_KEY@`. `install.sh` подставляет:

- `$ANT_LICENSE_KEY`, если переменная задана (коммерческий ключ);
- иначе — evaluation-ключ, найденный в `nrf_sdm.h` из скачанного S340.

Флаг передаётся в одинарных кавычках (`'-DANT_LICENSE_KEY="{build.ant_license_key}"'`) — так arduino-cli
сохраняет внутренние двойные кавычки (тот же приём ядро использует для `USB_MANUFACTURER`).

## Вариант nice!nano

Копия варианта Feather nRF52840 Express с отличиями:

- `USE_LFRC` вместо `USE_LFXO` — низкочастотный клок от внутреннего RC (работает независимо от того,
  распаян ли кварц 32 кГц);
- `LED_BUILTIN` (D3/D4) → **P0.15**, синий светодиод nice!nano;
- не трогается пин питания NeoPixel (у nice!nano его нет).

Нумерация остальных пинов совпадает с Feather — для этого проекта важны только USB и светодиод.

## Загрузка скетча

- **Upload в Arduino IDE** — штатно: 1200-baud touch переводит плату в загрузчик, `adafruit-nrfutil`
  шлёт пакет с `--sd-req 0x00CE` (FWID S340 7.0.1).
- **UF2**: собрать (`Sketch → Export Compiled Binary`) и сконвертировать
  `uf2conv.py firmware.hex -c -f 0xADA52840`, затем скопировать на диск `NICENANO`.

## После обновления ядра Adafruit

Boards Manager перезаписывает папку ядра — плата пропадёт. Просто запустите `install.sh` ещё раз.
