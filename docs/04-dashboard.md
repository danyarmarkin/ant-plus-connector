# 04 — Веб-страница

`hr_dashboard/server.py` читает Serial платы, раздаёт страницу и транслирует данные в браузер.
Зависимости: `pyserial` (для вентилятора — ещё `requests`, `pycryptodome`, `dnspython`).

```bash
python3 -m venv hr_dashboard/.venv
hr_dashboard/.venv/bin/pip install -r hr_dashboard/requirements.txt
hr_dashboard/.venv/bin/python hr_dashboard/server.py [--port /dev/cu.usbmodemXXXX] [--http 8765]
```

Открыть <http://localhost:8765>. Порт платы по умолчанию — первый `/dev/cu.usbmodem*`; при
отключении платы сервер ждёт и переподключается сам.

> Пока сервер запущен, он держит Serial-порт. Перед загрузкой скетча из Arduino IDE (и перед
> Serial Monitor) сервер нужно остановить.

## Что на странице

- **Пульс** крупно, сердце «бьётся» на каждый пакет; мин / сред / макс за выбранное окно, RR, RSSI, ID.
- **Датчики рядом** — все ANT+ ремни с уровнем сигнала и пульсом:
  «Подключить» — выбрать ремень, «Закрепить» — зафиксировать текущий, «Любой» — первый найденный.
- **Вентилятор** — состояние, ручные кнопки выкл/1/2/3, режим «плавно по пульсу», звук (см. [05](05-xiaomi-fan.md)).
- **График** пульса за 1 / 5 / 15 минут / 1 час (до ~16 000 точек в памяти сервера). Разрывы — нет контакта.

Страница — один HTML без внешних библиотек, светлая/тёмная тема по системе.

## API сервера

| Метод | Путь | |
|---|---|---|
| GET | `/` | страница |
| GET | `/history` | `{history: [...], status, sensors, fan}` — состояние на момент открытия |
| GET | `/events` | SSE-поток: `{type: "sample" \| "status" \| "sensors" \| "fan", ...}` |
| POST | `/pair` | `{"device": 12345}` или `{"device": 0}` (любой) → команда `pair` на плату |
| POST | `/fan` | `{"auto": true}`, `{"power": false}`, `{"power": true, "level": 2}`, `{"alarm": false}` |

Пример точки `sample`:

```json
{"type": "sample", "t": 1791214132.6, "state": "tracking", "hr": 84, "beats": 52, "rr": 705, "id": 12345, "rssi": -54}
```

`state`: `tracking` / `nocontact` (ремень на связи, но пульс 0) / `searching`.

Сервер слушает только `127.0.0.1`.
