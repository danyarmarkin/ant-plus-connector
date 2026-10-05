"""Control a Xiaomi fan through Xiaomi cloud (MIoT spec API) using the session saved by login.py.

Mi Smart Standing Fan 2 Lite = dmaker.fan.1c, MIoT properties (siid 2 = fan):
  piid 1  on            bool
  piid 2  fan-level     1..3
  piid 3  horizontal-swinging bool
  piid 7  mode          0 straight wind, 1 natural wind
  piid 11 alarm         bool, beep on commands
"""
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "xiaomi_extractor"))
_argv, sys.argv = sys.argv, [sys.argv[0]]  # token_extractor parses CLI args at import time
import token_extractor as te  # noqa: E402
sys.argv = _argv
from net import bind_session  # noqa: E402

SESSION_FILE = HERE / "session.json"

PROP_ON = (2, 1)
PROP_LEVEL = (2, 2)
PROP_SWING = (2, 3)
PROP_MODE = (2, 7)
PROP_ALARM = (2, 11)  # beep on commands


class SessionExpired(Exception):
    pass


class CloudFan:
    def __init__(self, device=None, session_file=SESSION_FILE):
        session = json.loads(Path(session_file).read_text())
        self.conn = te.QrCodeXiaomiCloudConnector()
        bind_session(self.conn._session)  # bypass VPN for Xiaomi cloud
        self.conn.userId = session["userId"]
        self.conn._ssecurity = session["ssecurity"]
        self.conn._serviceToken = session["serviceToken"]

        fans = [d for d in session["devices"] if d.get("model", "").startswith("dmaker.fan") or ".fan." in d.get("model", "")]
        if device:
            fans = [d for d in session["devices"] if device in (d.get("did"), d.get("name"))]
        if not fans:
            raise RuntimeError("No fan found in session.json; run login.py again")
        self.device = fans[0]
        self.did = self.device["did"]
        self.server = self.device["server"]
        self.name = self.device.get("name")
        self.model = self.device.get("model")

    def _call(self, path, params):
        url = self.conn.get_api_url(self.server) + path
        res = self.conn.execute_api_call_encrypted(url, {"data": json.dumps({"params": params})})
        if res is None:
            raise SessionExpired("Xiaomi cloud rejected the request (session expired?) -- run login.py again")
        if res.get("code") != 0:
            raise RuntimeError(f"Xiaomi cloud error: {res}")
        return res["result"]

    def get(self, *props):
        result = self._call("/miotspec/prop/get", [{"did": self.did, "siid": s, "piid": p} for s, p in props])
        return [r.get("value") if r.get("code") == 0 else None for r in result]

    def set(self, prop, value):
        s, p = prop
        result = self._call("/miotspec/prop/set", [{"did": self.did, "siid": s, "piid": p, "value": value}])
        code = result[0].get("code")
        if code not in (0, 1):  # 1 = accepted, applied asynchronously
            raise RuntimeError(f"set {prop}={value!r} failed: {result}")

    def status(self):
        on, level, swing, mode, alarm = self.get(PROP_ON, PROP_LEVEL, PROP_SWING, PROP_MODE, PROP_ALARM)
        return {"on": on, "level": level, "swing": swing, "mode": mode, "alarm": alarm}

    def power(self, on):
        self.set(PROP_ON, bool(on))

    def level(self, level):
        self.set(PROP_LEVEL, int(level))

    def set_alarm(self, on):
        self.set(PROP_ALARM, bool(on))


if __name__ == "__main__":
    fan = CloudFan()
    print(f"{fan.name} ({fan.model}) did={fan.did} server={fan.server}")
    if len(sys.argv) > 1:
        cmd = sys.argv[1]
        if cmd in ("on", "off"):
            fan.power(cmd == "on")
        elif cmd.isdigit():
            fan.level(int(cmd))
    print(fan.status())
