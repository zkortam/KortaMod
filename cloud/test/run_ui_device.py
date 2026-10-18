"""Turns the phone link on over USB and runs ui_device.mjs on the deployed site.

    python run_ui_device.py /dev/cu.usbmodem0123456783 <dir with puppeteer-core> [screenshot-dir]
The USB console stays open for the whole run (closing it can reset the ESP32,
which would change the pairing code)."""
import os
import re
import shutil
import subprocess
import sys
import time

import serial

port, modules = sys.argv[1], sys.argv[2]
shots = sys.argv[3] if len(sys.argv) > 3 else ""
con = serial.Serial()
con.port, con.baudrate, con.timeout = port, 115200, 0.2
con.dtr = con.rts = False
con.open()


def run(command, wait=0.8):
    con.reset_input_buffer()
    con.write((command + "\r\n").encode())
    time.sleep(wait)
    return con.read(16384).decode("utf-8", "replace")


def wait_for(command, pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        m = re.search(pattern, run(command))
        if m:
            return m
        time.sleep(1)
    sys.exit("timed out waiting for: " + pattern)


SITE = "https://kortamod.app"
wait_for("korta", r"korta wifi online", 60)
run("korta web on")
code = wait_for("korta web", r"ready 1 paired \d+ code (\d{6})", 60).group(1)
# ES modules resolve packages next to the script, so run a copy beside node_modules.
here = os.path.dirname(os.path.abspath(__file__))
shutil.copy(os.path.join(here, "ui_device.mjs"), os.path.join(modules, "ui_device.mjs"))
result = subprocess.run(["node", os.path.join(modules, "ui_device.mjs"), SITE, code, shots], cwd=modules)
con.close()
sys.exit(result.returncode)
