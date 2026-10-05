#!/usr/bin/env python3
"""Reads ant_hrm serial output from the nice!nano and streams it to a browser chart (SSE).

Usage: python server.py [--port /dev/cu.usbmodemXXXX] [--http 8765]
Needs: pip install pyserial (+ xiaomi_extractor/requirements.txt for fan control)
"""
import argparse
import glob
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial

HERE = Path(__file__).parent
sys.path.insert(0, str(HERE.parent / "xiaomi"))
HISTORY_LIMIT = 16000  # samples (~1 hour at 4 Hz)

# Heart rate -> continuous fan speed 1.0..3.0 (Mi Smart Standing Fan 2 Lite only has levels 1..3).
# Fractional speed is produced by alternating the two neighbouring levels within FAN_CYCLE_SEC.
FAN_HR_LOW = 90             # bpm at and below -> speed 1.0
FAN_HR_HIGH = 150           # bpm at and above -> speed 3.0
FAN_HR_SMOOTH_SEC = 10      # time constant of heart rate smoothing
FAN_CYCLE_SEC = 30          # duty cycle period for fractional speeds
FAN_MIN_DWELL = 0.15        # fractions closer than this to a whole level are rounded (no short blips)
FAN_MIN_CHANGE_SEC = 3      # never send level changes more often
FAN_OFF_AFTER_SEC = 30      # turn off when no valid heart rate for this long

RE_HR = re.compile(r"HR\s+(\d+) bpm")
RE_NO_CONTACT = re.compile(r"HR ---")
RE_BEATS = re.compile(r"beats\s+(\d+)")
RE_RR = re.compile(r"RR\s+(\d+) ms")
RE_ID = re.compile(r"ID (\d+)")
RE_RSSI = re.compile(r"RSSI (-?\d+) dBm")
RE_BATT = re.compile(r"batt (\d+)%")
RE_SENSOR = re.compile(r"(\d+) rssi (-?\d+) hr (\d+) age ([\d.]+)( \*)?")

history = []
clients = []
lock = threading.Lock()
status = {"serial": "disconnected", "port": None, "info": None}
sensors = {"target": None, "list": []}
serial_port = None              # open serial.Serial, used to send commands to the board
serial_write_lock = threading.Lock()
fan_state = {"available": False, "error": None, "name": None, "auto": False,
             "on": None, "level": None, "alarm": None, "speed": None, "hr_smooth": None, "last_change": 0.0}


def broadcast(msg):
    data = ("data: " + json.dumps(msg) + "\n\n").encode()
    with lock:
        for q in list(clients):
            q.append(data)


def parse_sensors(line):
    """SENSORS target=<n|any> | <dev> rssi <dBm> hr <bpm> age <s>[ *] | ..."""
    parts = line.split("|")
    target = parts[0].split("target=")[1].strip()
    sensors["target"] = None if target == "any" else int(target)
    sensors["list"] = []
    for part in parts[1:]:
        m = RE_SENSOR.search(part)
        if m:
            sensors["list"].append({"id": int(m.group(1)), "rssi": int(m.group(2)), "hr": int(m.group(3)),
                                    "age": float(m.group(4)), "paired": bool(m.group(5))})
    broadcast({"type": "sensors", **sensors})


def send_command(cmd):
    with serial_write_lock:
        if serial_port is None:
            raise RuntimeError("board not connected")
        serial_port.write((cmd + "\n").encode())


def parse_line(line):
    if line.startswith("SENSORS "):
        parse_sensors(line)
        return None
    if "SEARCHING" in line or "CLOSED for" in line:
        return {"t": time.time(), "state": "searching"}
    if line.lstrip().startswith("info:"):
        status["info"] = line.strip()[len("info:"):].strip()
        broadcast({"type": "status", **status})
        return None
    m = RE_HR.search(line)
    if not m and not RE_NO_CONTACT.search(line):
        return None
    s = {"t": time.time(), "state": "tracking" if m else "nocontact", "hr": int(m.group(1)) if m else None}
    for key, rx in (("beats", RE_BEATS), ("rr", RE_RR), ("id", RE_ID), ("rssi", RE_RSSI), ("batt", RE_BATT)):
        mm = rx.search(line)
        if mm:
            s[key] = int(mm.group(1))
    return s


def hr_to_speed(hr):
    """Linear map of heart rate to a continuous fan speed in [1, 3]."""
    k = (hr - FAN_HR_LOW) / (FAN_HR_HIGH - FAN_HR_LOW)
    return 1.0 + 2.0 * min(max(k, 0.0), 1.0)


def speed_to_level(speed, now):
    """Pick level 1..3 for this moment of the duty cycle so that the time average equals speed."""
    low = int(speed)
    frac = speed - low
    if frac < FAN_MIN_DWELL or low >= 3:
        return min(low, 3)
    if frac > 1 - FAN_MIN_DWELL:
        return low + 1
    phase = (now % FAN_CYCLE_SEC) / FAN_CYCLE_SEC
    return low + 1 if phase < frac else low


def fan_broadcast():
    broadcast({"type": "fan", **{k: v for k, v in fan_state.items() if k != "last_change"}})


fan = None
fan_lock = threading.Lock()


def fan_apply(on, level=None):
    """Send power/level to the fan if they differ from the known state."""
    with fan_lock:
        if on != fan_state["on"]:
            fan.power(on)
            fan_state["on"] = on
        if on and level is not None and level != fan_state["level"]:
            fan.level(level)
            fan_state["level"] = level
        fan_state["last_change"] = time.time()
        fan_state["error"] = None
    print(f"FAN -> on={on} level={fan_state['level']}")
    fan_broadcast()


def fan_loop():
    global fan
    try:
        from fan_cloud import CloudFan
        fan = CloudFan()
        st = fan.status()
        fan_state.update(available=True, name=fan.name, on=st["on"], level=st["level"], alarm=st.get("alarm"))
        print(f"Fan: {fan.name} ({fan.model}) {st}")
    except Exception as e:
        fan_state.update(available=False, error=str(e))
        print(f"Fan disabled: {e}")
        fan_broadcast()
        return
    fan_broadcast()

    last_valid_hr_t = time.time()
    last_tick = time.time()
    while True:
        time.sleep(1)
        now = time.time()
        dt, last_tick = now - last_tick, now
        with lock:
            last = history[-1] if history else None
        valid = last and last["state"] == "tracking" and last.get("hr") and now - last["t"] < 5

        # Exponential smoothing of heart rate (runs also in manual mode, for display)
        if valid:
            last_valid_hr_t = now
            hs = fan_state["hr_smooth"]
            alpha = min(1.0, dt / FAN_HR_SMOOTH_SEC)
            fan_state["hr_smooth"] = last["hr"] if hs is None else hs + alpha * (last["hr"] - hs)
            fan_state["speed"] = round(hr_to_speed(fan_state["hr_smooth"]), 2)
        if not fan_state["auto"]:
            continue
        try:
            if valid:
                level = speed_to_level(fan_state["speed"], now)
                if (not fan_state["on"] or level != fan_state["level"]) and now - fan_state["last_change"] >= FAN_MIN_CHANGE_SEC:
                    fan_apply(True, level)
            elif fan_state["on"] and now - last_valid_hr_t > FAN_OFF_AFTER_SEC:
                fan_state["hr_smooth"] = fan_state["speed"] = None
                fan_apply(False)
        except Exception as e:
            fan_state["error"] = str(e)
            fan_state["last_change"] = now  # back off before retrying
            print(f"Fan error: {e}")
        fan_broadcast()


def find_port(preferred):
    if preferred:
        return preferred
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def serial_loop(preferred_port):
    global serial_port
    while True:
        port = find_port(preferred_port)
        if not port:
            status.update(serial="waiting for board", port=None)
            broadcast({"type": "status", **status})
            time.sleep(1)
            continue
        try:
            with serial.Serial(port, 115200, timeout=1) as ser:
                serial_port = ser
                status.update(serial="connected", port=port)
                broadcast({"type": "status", **status})
                print(f"Reading {port}")
                while True:
                    raw = ser.readline()
                    if not raw:
                        continue
                    line = raw.decode(errors="replace").rstrip()
                    print(line)
                    sample = parse_line(line)
                    if sample:
                        with lock:
                            history.append(sample)
                            del history[:-HISTORY_LIMIT]
                        broadcast({"type": "sample", **sample})
        except (serial.SerialException, OSError) as e:
            serial_port = None
            status.update(serial=f"error: {e}", port=port)
            broadcast({"type": "status", **status})
            time.sleep(1)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            body = (HERE / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/history":
            with lock:
                body = json.dumps({"history": history, "status": status, "sensors": sensors,
                                   "fan": {k: v for k, v in fan_state.items() if k != "last_change"}}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            queue = []
            with lock:
                clients.append(queue)
            last_ping = 0.0
            try:
                while True:
                    while queue:
                        self.wfile.write(queue.pop(0))
                    if time.time() - last_ping > 5:
                        self.wfile.write(b": ping\n\n")
                        last_ping = time.time()
                    self.wfile.flush()
                    time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with lock:
                    clients.remove(queue)
        else:
            self.send_error(404)

    def _json(self, code, body):
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        if self.path == "/pair":
            try:
                req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
                dev = int(req.get("device") or 0)
                send_command(f"pair {dev}" if dev else "pair any")
                self._json(200, {"ok": True})
            except Exception as e:
                self._json(500, {"ok": False, "error": str(e)})
            return
        if self.path != "/fan":
            self.send_error(404)
            return
        try:
            req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
            if "auto" in req:
                fan_state["auto"] = bool(req["auto"])
                fan_broadcast()
            if not fan_state["available"]:
                raise RuntimeError(fan_state["error"] or "fan not available")
            if "alarm" in req:
                with fan_lock:
                    fan.set_alarm(bool(req["alarm"]))
                    fan_state["alarm"] = bool(req["alarm"])
                fan_broadcast()
            if "power" in req or "level" in req:
                fan_state["auto"] = False  # manual control disables auto mode
                fan_apply(bool(req.get("power", True)), req.get("level"))
            code, body = 200, {"ok": True}
        except Exception as e:
            fan_state["error"] = str(e)
            fan_broadcast()
            code, body = 500, {"ok": False, "error": str(e)}
        data = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--http", type=int, default=8765)
    args = ap.parse_args()
    threading.Thread(target=serial_loop, args=(args.port,), daemon=True).start()
    threading.Thread(target=fan_loop, daemon=True).start()
    print(f"Open http://localhost:{args.http}")
    ThreadingHTTPServer(("127.0.0.1", args.http), Handler).serve_forever()


if __name__ == "__main__":
    main()
