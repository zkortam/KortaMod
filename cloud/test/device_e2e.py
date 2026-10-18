"""End-to-end: a real Chromatic, the deployed site, and this script as the browser.

Opens the USB console (which resets the ESP32), turns the phone link on, reads
the pairing code, then uses https://kortamod.vercel.app exactly as a phone does.
The device's notes and alarm are saved first and restored at the end; the phone
link is left on. Browsers you paired yourself are never revoked unless you pass
--include-forget (which tests Forget phones and so unpairs everything). Opening
the console restarts the Chromatic, so paired pages show it offline briefly.

    python3 device_e2e.py /dev/cu.usbmodem0123456783 [https://kortamod.vercel.app]
"""
import json
import re
import sys
import time
import urllib.error
import urllib.request

import serial  # pyserial, from the ESP-IDF python env

ARGS = [a for a in sys.argv[1:] if not a.startswith("--")]
PORT = ARGS[0] if ARGS else "/dev/cu.usbmodem0123456783"
SITE = ARGS[1] if len(ARGS) > 1 else "https://kortamod.vercel.app"
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


class Console:
    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        self.s.dtr = self.s.rts = False
        self.s.open()

    def run(self, command, wait=0.8):
        self.s.reset_input_buffer()
        self.s.write((command + "\r\n").encode())
        time.sleep(wait)
        return self.s.read(16384).decode("utf-8", "replace")

    def wait(self, command, pattern, timeout):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.run(command))
            if m:
                return m
            time.sleep(1)
        return None


def http(method, path, body=None, token=None, raw=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(SITE + path, data=data, method=method)
    req.add_header("User-Agent", "kortamod-e2e")
    if data is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=45) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def state(token):
    return http("GET", "/api/state", token=token)[1]


def remote_control(con, token):
    """Settings, messages, timer and Wi-Fi from the site; everything restored."""
    st0 = state(token)["state"]
    settings, timer, wifi = st0.get("settings"), st0.get("timer"), st0.get("wifi")
    check(settings is not None and timer is not None and wifi is not None, "state includes settings, timer and Wi-Fi")
    if not settings:
        return
    b0 = settings["brightness"]
    b1 = b0 - 1 if b0 > 1 else b0 + 1
    try:
        t0 = time.time()
        st, _ = http("PUT", "/api/settings", {"key": "brightness", "value": b1}, token)
        check(st == 200 and state(token)["state"]["settings"]["brightness"] == b1,
              "brightness set from the site in %.1f s" % (time.time() - t0))
        st, _ = http("PUT", "/api/settings", {"key": "night", "value": not settings["night"]}, token)
        check(st == 200 and state(token)["state"]["settings"]["night"] == (not settings["night"]), "theme switched from the site")
        check(http("PUT", "/api/settings", {"key": "brightness", "value": 99}, token)[0] == 400, "out-of-range setting refused")

        st, _ = http("POST", "/api/message", {"text": "KortaMod E2E \u2014 hello"}, token)
        check(st == 200, "a message is shown on the Chromatic")
        con.run("korta web dismiss")
        st, _ = http("POST", "/api/message", {"text": "", "find": True}, token)
        check(st == 200 and state(token)["state"]["settings"]["brightness"] == b1,
              "find me flashes without losing the user's brightness")
        con.run("korta web dismiss")

        st, _ = http("POST", "/api/timer", {"action": "set", "countdown": True, "seconds": 300}, token)
        t = state(token)["state"]["timer"]
        check(st == 200 and t["countdown"] and t["set_s"] == 300, "timer set to 5 minutes from the site")
        http("POST", "/api/timer", {"action": "start"}, token)
        a = state(token)["state"]["timer"]
        time.sleep(2.5)
        b = state(token)["state"]["timer"]
        check(a["running"] and b["ms"] < a["ms"], "the timer runs on the Chromatic (%d -> %d ms)" % (a["ms"], b["ms"]))
        check(http("POST", "/api/timer", {"action": "set", "countdown": True, "seconds": 60}, token)[0] == 423,
              "a running timer can't be re-set")
        http("POST", "/api/timer", {"action": "stop"}, token)
        http("POST", "/api/timer", {"action": "reset"}, token)
        check(not state(token)["state"]["timer"]["running"], "timer stopped and reset")

        home = wifi["ssid"]
        t0 = time.time()
        st, _ = http("POST", "/api/wifi", {"ssid": "kortamod-e2e-missing", "password": "not-a-real-net"}, token)
        check(st == 200, "a Wi-Fi change is answered before the Chromatic switches")
        left = back = False
        while time.time() - t0 < 180 and not back:
            code_, s_ = http("GET", "/api/state", token=token)
            if code_ == 200:
                stale = s_["now"] - s_["seen"] > 4000
                left = left or stale or not s_["online"]
                w = s_.get("state", {}).get("wifi", {})
                back = left and not stale and s_["online"] and w.get("ssid") == home and w.get("phase") == "online"
            time.sleep(2)
        check(left, "the Chromatic really left %s to try the other network" % home)
        check(back, "it couldn't join, so it came back to %s by itself (%.0f s)" % (home, time.time() - t0))
    finally:
        http("PUT", "/api/settings", {"key": "brightness", "value": b0}, token)
        http("PUT", "/api/settings", {"key": "night", "value": settings["night"]}, token)
        if timer:
            http("POST", "/api/timer", {"action": "set", "countdown": timer["countdown"], "seconds": timer["set_s"]}, token)
        after = state(token)["state"]
        check(after["settings"]["brightness"] == b0 and after["settings"]["night"] == settings["night"]
              and after["timer"]["set_s"] == timer["set_s"], "your settings and timer are restored")


def main():
    con = Console(PORT)
    print("Waiting for Wi-Fi and the phone link...", flush=True)
    check(con.wait("korta", r"korta wifi online", 60) is not None, "Chromatic joins Wi-Fi after reset")
    con.run("korta web on")
    m = con.wait("korta web", r"ready 1 paired (\d+) code (\d{6}) host (\S+) last 200", 60)
    check(m is not None, "the Chromatic connects to the site")
    if not m:
        print(con.run("korta web"))
        return
    code, host = m.group(2), m.group(3)
    check(host == SITE.split("//")[1], "the Chromatic shows %s" % host)
    print("  code", code, flush=True)

    st, _ = http("POST", "/api/pair", {"code": "000000" if code != "000000" else "111111"})
    check(st == 403, "a wrong code is refused")
    st, body = http("POST", "/api/pair", {"code": code[:3] + " " + code[3:]})
    token = body.get("token", "")
    check(st == 200 and re.fullmatch(r"[0-9a-f]{64}", token) is not None, "the right code pairs")
    check(http("POST", "/api/pair", {"code": code})[0] == 403, "a code works only once")
    t0 = time.time()
    fresh = None
    while time.time() - t0 < 20 and not fresh:
        m2 = re.search(r"paired (\d+) code (\d{6})", con.run("korta web"))
        if m2 and m2.group(2) != code and int(m2.group(1)) >= 1:
            fresh = m2
    check(fresh is not None, "the Chromatic shows a fresh code and counts the pairing (%.1f s)" % (time.time() - t0))

    s0 = state(token)
    check(s0.get("online") is True and s0.get("state", {}).get("notes") is not None, "state: online, with notes")
    saved_notes = s0["state"]["notes"]["items"]
    saved_alarms = s0["state"]["alarms"]
    saved_pinned = s0["state"]["notes"].get("pinned", 0)
    rev = s0["state"]["notes"]["rev"]
    try:
        check(http("PUT", "/api/notes", {"rev": (rev + 1) % 65536, "items": ["x"]}, token)[0] == 409,
              "a stale revision is refused")
        t0 = time.time()
        st, _ = http("PUT", "/api/notes", {"rev": rev, "items": ["“E2E” café — cloud \U0001F600", "", "second"]}, token)
        took = time.time() - t0
        check(st == 200, "notes saved through the cloud in %.1f s" % took)
        items = state(token)["state"]["notes"]["items"]
        check(items == ['"E2E" cafe - cloud ?', "second"], "cleaned on the device and visible right away: %r" % items)
        check(http("PUT", "/api/notes", {"rev": 0, "items": ["a"] * 17}, token)[0] == 400, "more than 16 notes refused")
        rev2 = state(token)["state"]["notes"]["rev"]
        st, _ = http("PUT", "/api/notes", {"rev": rev2, "items": ["pinned one", "plain"], "pinned": 1}, token)
        n = state(token)["state"]["notes"]
        check(st == 200 and n["pinned"] == 1 and n["items"] == ["pinned one", "plain"] and n["slots"] == 16,
              "a pinned note is kept pinned on the Chromatic")
        check(http("PUT", "/api/notes", token=token, raw=b"{bad")[0] == 400, "malformed JSON refused")
        check(http("PUT", "/api/alarm", {"index": 0, "on": True, "hour": 24, "minute": 0}, token)[0] == 400, "impossible alarm refused")
        t0 = time.time()
        want = {"on": False, "hour": 6, "minute": 30, "days": 31, "label": "E2E caf\u00e9"}
        st, _ = http("PUT", "/api/alarm", dict(want, index=1), token)
        got = state(token)["state"]["alarms"]
        check(st == 200 and len(got) == 4 and got[1] == dict(want, label="E2E cafe"),
              "alarm 2 set with label and weekdays through the cloud in %.1f s" % (time.time() - t0))
        zone = -(time.altzone if time.localtime().tm_isdst else time.timezone) // 60
        st, _ = http("PUT", "/api/clock", {"zone_minutes": zone}, token)
        clock = state(token)["state"]["clock"]
        check(st == 200 and clock["known"] and abs(clock["epoch"] - time.time()) < 15 and clock["zone_minutes"] == zone,
              "clock and zone set from the server's time (%s)" % clock)
        remote_control(con, token)
    finally:
        rev_now = state(token)["state"]["notes"]["rev"]
        st1, _ = http("PUT", "/api/notes", {"rev": rev_now, "items": saved_notes, "pinned": saved_pinned}, token)
        st2 = [http("PUT", "/api/alarm", dict(a, index=i), token)[0] for i, a in enumerate(saved_alarms)]
        after = state(token)["state"]
        check(st1 == 200 and after["notes"]["items"] == saved_notes, "your notes are restored")
        check(st2 == [200] * 4 and after["alarms"] == saved_alarms, "your alarms are restored")

    check(http("POST", "/api/unpair", token=token)[0] == 200 and http("GET", "/api/state", token=token)[0] == 401,
          "unpairing locks this browser out")
    if "--include-forget" not in sys.argv:
        print("  skip Forget phones (it would unpair every real browser too; pass --include-forget)")
        con.run("korta web on")
        con.s.close()
        return
    code2 = con.wait("korta web", r"code (\d{6})", 10).group(1)
    st, body = http("POST", "/api/pair", {"code": code2})
    other = body.get("token", "")
    check(st == 200, "a second browser pairs with the new code")
    con.run("korta web forget")
    gone = False
    for _ in range(20):
        if http("GET", "/api/state", token=other)[0] == 401:
            gone = True
            break
        time.sleep(1)
    check(gone, "Forget phones on the Chromatic revokes every browser")
    m3 = con.wait("korta web", r"paired 0 ", 20)
    check(m3 is not None, "the Chromatic shows no paired browsers")

    con.run("korta web on")
    print(con.run("korta").strip().splitlines()[-1:])
    con.s.close()


if __name__ == "__main__":
    main()
    print("\n%d failure(s)" % len(failures))
    for f in failures:
        print("  - " + f)
    sys.exit(1 if failures else 0)
