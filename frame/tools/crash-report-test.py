#!/usr/bin/env python3
"""Tests frame/tools/crash-report.py without a headset or a network: a fake crashed session is prepared,
checked for scrubbing and classification, then sent to a Sentry stand-in on 127.0.0.1.

  python3 frame/tools/crash-report-test.py
"""
import http.server
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "crash-report.py")
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        failures.append(what)


def run(env, *args):
    return subprocess.run([sys.executable, "-I", TOOL, *args], env=env, capture_output=True, text=True)


def main():
    work = tempfile.mkdtemp(prefix="kf-crash-test-")
    home = os.path.join(work, "home", "kartfan")
    session, runlog, app, out = (os.path.join(work, d) for d in ("session", "runlog", "app", "out"))
    for d in (home, session, runlog, os.path.join(app, "kit")):
        os.makedirs(d)
    host = socket.gethostname().split(".")[0]
    env = dict(os.environ, HOME=home, USERPROFILE=home, USER="kartfan", LOGNAME="kartfan", USERNAME="kartfan")
    env.pop("KARTFRAME_SENTRY_DSN", None)

    with open(os.path.join(session, "game.log"), "w") as f:
        f.write(f"[runtime] KartFrame 1.0.1\n[aurora] Graphics adapter information\n  API: Vulkan\n"
                f"  Device: Turnip Adreno (TM) 750 (IntegratedGPU)\n  Driver: Mesa 25.1.0\n"
                f"config {home}/.local/share/KartFrame/UserData/Config.toml by kartfan on {host}\n"
                f"peer 192.168.1.23 and fe80::1:2:3:4 mac 12:34:56:78:9a:bc steam 76561198000000001\n"
                f"[runtime] Sentry test error 1700000000 (KARTFRAME_SENTRY_TEST_CRASH)\n"
                f"[runtime] Signal 11 (fault address 0x8)\n")
    with open(os.path.join(session, "launch.log"), "w") as f:
        f.write("10:00:00 refresh rate 90 Hz (default)\n10:00:00 eye size asked of SteamVR: 1728 (SteamVR: Auto)\n")
    with open(os.path.join(runlog, "crash_sigsegv.txt"), "w") as f:
        f.write(f"[runtime] fatal log\n[runtime] reason: sigsegv\n[runtime] host stack trace (most recent call first):\n"
                f"  #00 pc 0x0000aaaab0001234 kartframe+0x1234\n  #01 pc 0x0000aaaab0005678 kartframe+0x5678\n"
                f"[runtime] host images:\n  image 0xaaaab0000000 0x2000000 0123456789abcdef0123456789abcdef01234567 "
                f"{home}/.local/share/KartFrame/bin/kartframe\n")
    with open(os.path.join(runlog, "crash_exitcode.txt"), "w") as f:
        f.write("[runtime] fatal log\n[runtime] reason: exitcode\n")
    with open(os.path.join(app, "VERSION"), "w") as f:
        f.write("1.0.1\n")
    with open(os.path.join(app, "kit", "kit.json"), "w") as f:
        f.write('{"fingerprint": "abcdef0123456789"}')
    with open(os.path.join(work, "Config.toml"), "w") as f:
        f.write("[vr]\ndefault_camera = 1\n")

    common = ["--session", session, "--run-log", runlog, "--app", app, "--config", os.path.join(work, "Config.toml"), "--out", out]
    r = run(env, "prepare", "--status", "143", *common)
    check(r.returncode == 0, "a crash file counts even when Steam stopped the game afterwards")
    check("sigsegv" in open(os.path.join(out, "event.json")).read(), "the signal's crash file wins over crash_exitcode.txt")
    crash_file = open(os.path.join(runlog, "crash_sigsegv.txt")).read()
    shutil.rmtree(out)
    shutil.rmtree(runlog); os.makedirs(runlog)
    r = run(env, "prepare", "--status", "143", *common)
    check(r.returncode == 3 and not os.path.exists(out), "SIGTERM without a crash file is no crash")
    r = run(env, "prepare", "--status", "0", *common)
    check(r.returncode == 3, "a clean exit is no crash")
    r = run(env, "prepare", "--status", "139", *common)
    check(r.returncode == 0 and "SIGSEGV" in open(os.path.join(out, "summary.txt")).read(), "exit status 139 is SIGSEGV")
    check(not os.path.exists(os.path.join(out, "sendable")), "no DSN: no Send Report")

    # The real crash file again, with a DSN pointing at the stand-in.
    with open(os.path.join(runlog, "crash_sigsegv.txt"), "w") as f:
        f.write(crash_file)
    received = {}

    class Sentry(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            received["path"] = self.path
            received["auth"] = self.headers.get("X-Sentry-Auth", "")
            received["body"] = self.rfile.read(int(self.headers["Content-Length"]))
            self.send_response(200); self.end_headers(); self.wfile.write(b'{"id":"x"}')

        def log_message(self, *a):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Sentry)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env["KARTFRAME_SENTRY_DSN"] = f"http://publickey@127.0.0.1:{server.server_port}/42"
    r = run(env, "prepare", "--status", "1", *common)
    check(r.returncode == 0, "prepare with a crash file")
    check(os.path.exists(os.path.join(out, "sendable")), "a DSN: Send Report offered")
    shown = open(os.path.join(out, "whats-sent.txt"), encoding="utf-8").read()
    for secret, what in ((home, "home folder"), ("kartfan", "user name"), ("192.168.1.23", "IPv4"), ("fe80::1:2:3:4", "IPv6"),
                         ("12:34:56:78:9a:bc", "MAC"), ("76561198000000001", "Steam id")):
        check(secret not in shown, f"{what} scrubbed")
    if len(host) >= 3 and host.lower() not in ("localhost", "steamos", "deck"):
        check(host not in shown, "host name scrubbed")
    check("~/.local/share/KartFrame" in shown, "home folder shown as ~")
    event = json.load(open(os.path.join(out, "event.json")))
    check(event["release"] == "kartframe@1.0.1" and event["environment"] == "production", "release and environment")
    check(event["tags"].get("gpu", "").startswith("Turnip") and event["tags"].get("driver") == "Mesa 25.1.0", "GPU and driver tags")
    check(event["tags"].get("refresh_hz") == "90" and event["tags"].get("camera") == "first person", "VR tags")
    frames = event["exception"]["values"][0]["stacktrace"]["frames"]
    check([f["instruction_addr"] for f in frames] == ["0xaaaab0005678", "0xaaaab0001234"], "stack oldest first")
    img = event["debug_meta"]["images"][0]
    check(img["debug_id"] == "67452301-ab89-efcd-0123-456789abcdef" and img["image_addr"] == "0xaaaab0000000", "debug image")
    check("Sentry test error" in event["exception"]["values"][0]["value"], "test crash named in the event")
    check("server_name" not in event and event["sdk"]["settings"]["infer_ip"] == "never", "no server name, no IP inferred")

    r = run(env, "send", out, "--app", app)
    check(r.returncode == 0 and "sent" in r.stdout, "send: " + r.stdout.strip())
    check(received.get("path") == "/api/42/envelope/" and "sentry_key=publickey" in received.get("auth", ""), "envelope endpoint and key")
    lines = received.get("body", b"").split(b"\n")
    header = json.loads(lines[0]) if lines and lines[0] else {}
    check(header.get("dsn", "").startswith("http://publickey@"), "envelope header")
    items = [json.loads(l).get("filename") for l in lines[1:] if l.startswith(b'{"type": "attachment"')]
    check(items == ["report.txt", "game-log.txt", "launch-log.txt"], "three attachments")
    server.shutdown()
    shutil.rmtree(work, ignore_errors=True)
    print(f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
