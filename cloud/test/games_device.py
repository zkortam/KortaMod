"""Games end to end: install a game through the site, run it on the Chromatic,
capture its screen over USB video, quit and uninstall it.

    python3 games_device.py /dev/cu.usbmodem0123456783 game.js [screenshot.png]
"""
import hashlib, json, re, subprocess, sys, time

def status_line(text):
    lines = [l for l in text.replace("\r", "").splitlines() if l.startswith("korta games")]
    return lines[-1] if lines else text.strip()
sys.path.insert(0, __import__("os").path.dirname(__file__))
import device_e2e as e
e.SITE = "https://kortamod.app"

port, game_file = sys.argv[1], sys.argv[2]
shot = sys.argv[3] if len(sys.argv) > 3 else ""
source = open(game_file).read()
con = e.Console(port)
e.check(con.wait("korta", r"korta wifi online", 60) is not None, "Chromatic online")
m = con.wait("korta web", r"ready 1 paired (\d+) code (\d{6})", 60)
st, body = e.http("POST", "/api/pair", {"code": m.group(2)})
tok = body["token"]
e.check(st == 200, "paired")

g0 = con.run("korta game")
e.check("ready 1" in g0, "the games storage exists (new flash layout): " + status_line(g0))
before = e.state(tok)["state"].get("games", [])
before_slots = [g["slot"] for g in before]
before_slots_keep = [g["slot"] for g in before if g["name"] != "Snake"]   # the user's own games stay
t0 = time.time()
st, body = e.http("POST", "/api/games", {"name": "Snake", "source": source}, tok)
e.check(st == 200, "installed through the site in %.1f s (%s)" % (time.time() - t0, body))
if st != 200:
    print("  device:", con.run("korta game", 1.5).strip().replace("\r", ""))
    sys.exit(1)
games = e.state(tok)["state"].get("games", [])
e.check(any(g["name"] == "Snake" and g["size"] == len(source.encode()) for g in games), "state lists it: %s" % games)
listing = con.run("korta game")
idx = None
for i, mm in enumerate(re.finditer(r"\[(\d+):([^:]*):(\d+)\]", listing)):
    if mm.group(2) == "Snake": idx = i
e.check(idx is not None, "the Chromatic lists it: " + status_line(listing))

con.run("korta game %d" % idx, 1.0)
ok = con.wait("korta game", r"phase 1 state 3", 12)   # arcade running, game task running
out = con.run("korta game")
e.check(ok is not None, "the game loaded and runs on the device: " + status_line(out))
if shot:
    con.run("korta game show", 1.0)   # the Menu will open on KortaMod's tab
    con.run("poke 100", 1.5)          # open the Menu so the game is drawn
    time.sleep(3)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "avfoundation", "-pixel_format", "uyvy422", "-framerate", "60.00024",
                    "-video_size", "160x144", "-i", "0", "-frames:v", "1", shot], check=False)
    print("  screen captured:", shot)
    print("  " + status_line(con.run("korta game")))
    con.run("poke 100", 1.0)   # close it again
con.run("korta game quit", 1.5)
q = con.wait("korta game", r"state 0", 10)
e.check(q is not None, "quitting frees the game: " + status_line(con.run("korta game")))
for _ in range(60):  # Wi-Fi rejoins and the phone link resumes
    s_ = e.http("GET", "/api/state", token=tok)[1]
    if s_.get("online") and s_.get("now", 0) - s_.get("seen", 0) < 5000: break
    time.sleep(2)
ours = [g["slot"] for g in e.state(tok)["state"].get("games", []) if g["slot"] not in before_slots]
e.check(len(ours) == 1, "exactly one new game slot: %s" % ours)
st = [e.http("DELETE", "/api/games", {"slot": s_}, tok)[0] for s_ in ours]
left = [g["slot"] for g in e.state(tok)["state"].get("games", [])]
e.check(st == [200] * len(ours) and not any(s_ in left for s_ in ours), "uninstalled through the site (%s)" % st)
for g in e.state(tok)["state"].get("games", []):   # leftovers from interrupted runs
    if g["name"] == "Snake" and g["slot"] not in before_slots_keep:
        e.http("DELETE", "/api/games", {"slot": g["slot"]}, tok)
e.http("POST", "/api/unpair", token=tok)
con.s.close()
print("\n%d failure(s)" % len(e.failures))
for f in e.failures: print("  - " + f)
