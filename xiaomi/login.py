#!/usr/bin/env python3
"""Log in to Xiaomi cloud by QR code (scan with the Mi Home app) and save the session + device list.

The QR code is served at http://127.0.0.1:31415 while waiting.
Result: xiaomi/session.json (chmod 600) -- contains session tokens, keep it private.
No password is entered or stored.
"""
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "xiaomi_extractor"))
_argv, sys.argv = sys.argv, [sys.argv[0]]  # token_extractor parses CLI args at import time
import token_extractor as te  # noqa: E402
from net import bind_session  # noqa: E402
sys.argv = _argv

SESSION_FILE = HERE / "session.json"


def collect_devices(connector, servers):
    found = []
    for server in servers:
        homes = []
        try:
            res = connector.get_homes(server)
            if res and res.get("result"):
                for h in res["result"].get("homelist") or []:
                    homes.append((h["id"], connector.userId))
            cnt = connector.get_dev_cnt(server)
            if cnt and cnt.get("result"):
                for h in cnt["result"]["share"]["share_family"] or []:
                    homes.append((h["home_id"], h["home_owner"]))
        except Exception as e:  # some regions reject the account
            print(f"  {server}: {type(e).__name__}")
            continue
        for home_id, owner in homes:
            devs = connector.get_devices(server, home_id, owner)
            for d in (devs or {}).get("result", {}).get("device_info") or []:
                found.append({
                    "server": server,
                    "did": d.get("did"),
                    "name": d.get("name"),
                    "model": d.get("model"),
                    "localip": d.get("localip"),
                    "token": d.get("token"),
                    "isOnline": d.get("isOnline"),
                })
    return found


def main():
    connector = te.QrCodeXiaomiCloudConnector()
    bind_session(connector._session)  # bypass VPN: ru.api.io.mi.com is unreachable through it
    if "--refresh" in sys.argv:
        # Reuse the saved session, only re-fetch the device list
        old = json.loads(SESSION_FILE.read_text())
        connector.userId, connector._ssecurity, connector._serviceToken = old["userId"], old["ssecurity"], old["serviceToken"]
    else:
        print("Open http://127.0.0.1:31415 and scan the QR code with the Mi Home app (Profile -> scan).", flush=True)
        if not connector.login():
            print("Login failed")
            sys.exit(1)
        print("Logged in, fetching devices...", flush=True)

    servers = [a.split("=", 1)[1] for a in sys.argv if a.startswith("--server=")] or te.SERVERS
    devices = collect_devices(connector, servers)
    if "--refresh" in sys.argv:  # keep devices from regions that were not re-checked
        devices += [d for d in old["devices"] if d["server"] not in servers]
    session = {
        "userId": connector.userId,
        "ssecurity": connector._ssecurity,
        "serviceToken": connector._serviceToken,
        "devices": devices,
    }
    SESSION_FILE.write_text(json.dumps(session, indent=2, ensure_ascii=False))
    os.chmod(SESSION_FILE, 0o600)

    print(f"Saved {SESSION_FILE}")
    for d in devices:
        print(f"  [{d['server']}] {d['name']!r} model={d['model']} ip={d['localip']} online={d['isOnline']}")


if __name__ == "__main__":
    main()
