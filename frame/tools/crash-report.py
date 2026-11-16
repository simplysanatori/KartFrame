#!/usr/bin/env python3
"""KartFrame crash reports: prepares the report of a run that crashed, and sends it to Sentry when, and
only when, the player chose Send Report in the game.

  crash-report.py prepare --session DIR --status N [--run-log DIR] [--app DIR] [--config FILE] --out DIR
  crash-report.py send DIR [--app DIR]
  crash-report.py can-send [--app DIR]

prepare (the launch wrapper, after the game exits): decides from the exit status and the game's
crash_<reason>.txt files whether the run crashed. If it did, it writes into --out, replacing an
unanswered report from before: report.txt, game-log.txt (the last 2,000 lines of game.log),
launch-log.txt, event.json (the Sentry event), whats-sent.txt (all of it, as the game shows it under
See What's Sent), summary.txt (one line for the question) and, when there is a DSN, sendable. Every
file is scrubbed first: the home folder becomes ~, the user name <user>, the host name <host>, IPv4 and
IPv6 addresses <ip>, MAC addresses <mac>, plus e-mail addresses, Steam ids and tokens. Exit 0 with a
report, 3 when the run did not crash.

send (the launch wrapper, when the game wrote "send" into DIR/answer): posts that one report as one
Sentry envelope (the event and the three text files as attachments) to the DSN's host and nothing
else. No Sentry SDK, no session, no background traffic. Exit 0 when Sentry accepted it.

The DSN: KARTFRAME_SENTRY_DSN, else <app>/tools/sentry-dsn (written by frame/release.sh), else
~/.config/kartframe-dev/sentry-dsn (the build machine). Without one there is no Send Report.
The release: kartframe@<app>/VERSION (environment production), else kartframe@<git short sha> from
<app>/tools/build-commit or the repository (environment development).
"""
import argparse
import datetime
import getpass
import json
import os
import re
import socket
import ssl
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import uuid

LOG_LINES = 2000
SIGNALS = {4: "SIGILL", 5: "SIGTRAP", 6: "SIGABRT", 7: "SIGBUS", 8: "SIGFPE", 9: "SIGKILL", 11: "SIGSEGV", 14: "SIGALRM"}
NOT_CRASHES = {0, 128 + 1, 128 + 2, 128 + 15}  # a clean exit, SIGHUP, SIGINT, SIGTERM (Steam stopping the game)
CAMERAS = {"0": "original", "1": "first person", "2": "diorama"}
GENERIC_NAMES = {"steamos", "deck", "root", "user", "steamframe", "frame", "steamdeck", "localhost"}


# ---- scrubbing --------------------------------------------------------------------------------

class Scrubber:
    def __init__(self):
        self.home = os.path.expanduser("~").rstrip("/\\")
        try:
            self.user = getpass.getuser()
        except Exception:
            self.user = ""
        self.host = socket.gethostname().split(".")[0]
        self.rules = [
            (re.compile(r"-----BEGIN [A-Z ]+-----.*?-----END [A-Z ]+-----", re.S), "<pem removed>"),
            (re.compile(r"\b(?:(?:ghp|gho|ghu|ghs|ghr|github_pat|xox[abp])_|sk-)[A-Za-z0-9_-]{16,}"), "<token>"),
            (re.compile(r"\b[\w.+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}\b"), "<email>"),
            (re.compile(r"(?<![0-9A-Fa-f:])[0-9A-Fa-f]{2}([:-])[0-9A-Fa-f]{2}(?:\1[0-9A-Fa-f]{2}){4}(?![0-9A-Fa-f:])"), "<mac>"),
            (re.compile(r"(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}(?![0-9A-Fa-f:])"
                        r"|(?<![0-9A-Fa-f:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}:){0,5}[0-9A-Fa-f]{1,4}(?![0-9A-Fa-f:])"), "<ip>"),
            (re.compile(r"(?<![\w.])(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)(?![\w.])"), self._ipv4),
            (re.compile(r"\b7656119\d{10}\b"), "<steamid>"),
            (re.compile(r"(userdata/)\d+"), r"\1<id>"),
            (re.compile(r"/home/(?!<)[^/\s]+"), "/home/<user>"),
        ]

    @staticmethod
    def _ipv4(m):
        s = m.group(0)
        return s if s.startswith("127.") or s == "0.0.0.0" else "<ip>"

    def __call__(self, text):
        if self.home and len(self.home) > 1:
            text = text.replace(self.home + "/", "~/").replace(self.home + "\\", "~\\")
            text = re.sub(re.escape(self.home) + r"(?![\w.-])", "~", text)
        for pattern, repl in self.rules:
            text = pattern.sub(repl, text)
        for name, mask in ((self.user, "<user>"), (self.host, "<host>")):
            if len(name) >= 3 and name.lower() not in GENERIC_NAMES:
                text = re.sub(r"(?<![\w])" + re.escape(name) + r"(?![\w])", mask, text, flags=re.I)
        return text


# ---- reading the run --------------------------------------------------------------------------

def read(path, limit=None):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return ""
    return text if limit is None else text[:limit]


def tail(path, lines):
    return "".join(read(path).splitlines(keepends=True)[-lines:])


def app_release(app):
    version = read(os.path.join(app, "VERSION")).strip().splitlines()[:1] if app else []
    if version and version[0]:
        return version[0], "kartframe@" + version[0], "production"
    sha = read(os.path.join(app, "tools", "build-commit")).strip() if app else ""
    if not sha:
        for where in filter(None, (app, os.path.dirname(os.path.abspath(__file__)))):
            try:
                sha = subprocess.run(["git", "-C", where, "rev-parse", "--short", "HEAD"], capture_output=True,
                                     text=True, timeout=5).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                sha = ""
            if sha:
                break
    return "development", "kartframe@" + (sha or "unknown"), "development"


def find_dsn(app):
    candidates = [os.environ.get("KARTFRAME_SENTRY_DSN", "")]
    if app:
        candidates.append(read(os.path.join(app, "tools", "sentry-dsn")))
    candidates.append(read(os.path.expanduser("~/.config/kartframe-dev/sentry-dsn")))
    for c in candidates:
        c = c.strip().splitlines()[0].strip() if c.strip() else ""
        if parse_dsn(c):
            return c
    return ""


def parse_dsn(dsn):
    """(envelope url, public key) of a DSN https://KEY@HOST/PROJECT, or None."""
    u = urllib.parse.urlsplit(dsn)
    project = u.path.rstrip("/").rsplit("/", 1)[-1]
    if u.scheme not in ("https", "http") or not u.username or not u.hostname or not project.isdigit():
        return None
    prefix = u.path.rstrip("/")[: -len(project)].rstrip("/")
    port = f":{u.port}" if u.port else ""
    return f"{u.scheme}://{u.hostname}{port}{prefix}/api/{project}/envelope/", u.username


def os_info():
    info = {}
    for line in read("/etc/os-release").splitlines():
        k, _, v = line.partition("=")
        info[k] = v.strip().strip('"')
    return info


def config_value(path, section, key):
    try:
        import tomllib
        with open(path, "rb") as f:
            v = tomllib.load(f).get(section, {}).get(key)
        return None if v is None else str(v)
    except Exception:
        return None


def parse_stack(crash_text):
    """Frames (newest first) and modules from the game's host stack section (runtime/src/main.cpp)."""
    frames = [int(m.group(1), 16) for m in re.finditer(r"^\s*#\d+ pc 0x([0-9a-fA-F]+)", crash_text, re.M)]
    modules = []
    for m in re.finditer(r"^\s*image 0x([0-9a-fA-F]+) 0x([0-9a-fA-F]+) (\S+) (.*)$", crash_text, re.M):
        modules.append({"start": int(m.group(1), 16), "size": int(m.group(2), 16),
                        "build_id": "" if m.group(3) == "-" else m.group(3).lower(), "path": m.group(4).strip()})
    return frames, modules


def debug_id(build_id):
    """Sentry's debug id of an ELF file: the first 16 bytes of the GNU build id as a little-endian GUID."""
    b = bytes.fromhex(build_id)[:16].ljust(16, b"\0")
    return str(uuid.UUID(bytes_le=b))


# ---- prepare ----------------------------------------------------------------------------------

def classify(status, crash_files):
    """(reason tag, exception type, sentence) for a crash, or None."""
    if crash_files:
        reason = crash_files[0][0]
        if reason in ("sigsegv", "sigbus", "sigabrt"):
            return reason, reason.upper(), f"The game crashed ({reason.upper()})."
        return reason, "FatalError", f"The game stopped after a fatal error ({reason})."
    if status in NOT_CRASHES:
        return None
    if status > 128:
        name = SIGNALS.get(status - 128, f"signal {status - 128}")
        if status - 128 == 9:
            return "sigkill", name, "The game was killed (SIGKILL; often the system running out of memory)."
        return name.lower().replace(" ", ""), name, f"The game crashed ({name})."
    return "exit", "ExitStatus", f"The game stopped with exit status {status}."


def prepare(a):
    scrub = Scrubber()
    crash_files = []
    if a.run_log and os.path.isdir(a.run_log):
        for name in sorted(os.listdir(a.run_log)):
            m = re.fullmatch(r"crash_(\w+)\.txt", name)
            if m:
                crash_files.append((m.group(1), read(os.path.join(a.run_log, name), 256 * 1024)))
        # The signal handlers write the most telling file; later exit reports only repeat it.
        crash_files.sort(key=lambda f: f[0] in ("exitcode", "exit", "exception", "terminate"))
    kind = classify(a.status, crash_files)
    if kind is None:
        return 3
    reason, exc_type, sentence = kind

    game_log_path = os.path.join(a.session, "game.log")
    game_log = scrub(tail(game_log_path, LOG_LINES))
    launch_log = scrub(read(os.path.join(a.session, "launch.log"), 256 * 1024))
    crash_text = scrub(crash_files[0][1]) if crash_files else ""
    version, release, environment = app_release(a.app)
    osr = os_info()
    gpu = re.search(r"^\s+Device: (.+)$", game_log, re.M)
    driver = re.search(r"^\s+Driver: (.+)$", game_log, re.M)
    hz = re.search(r"refresh rate (\d+) Hz", launch_log)
    eye = re.search(r"eye size asked of SteamVR: (\d+)", launch_log)
    camera = CAMERAS.get(config_value(a.config, "vr", "default_camera") or "", None) if a.config else None
    vr = "desktop" if "continuing with desktop rendering" in game_log else "openxr"
    kit = ""
    if a.app:
        m = re.search(r'"fingerprint"\s*:\s*"([0-9a-f]{12})', read(os.path.join(a.app, "kit", "kit.json")))
        kit = m.group(1) if m else ""
    test = re.search(r"Sentry test error [^\n(]+", game_log)
    details = re.search(r"^\[runtime\] details: (.+)$", crash_text, re.M)
    fault = re.search(r"Signal \d+ \(fault address (0x[0-9a-f]+)\)", game_log)
    value = (details.group(1)[:300] if details else sentence)
    if fault and exc_type in ("SIGSEGV", "SIGBUS"):
        value = f"{exc_type} at address {fault.group(1)}"
    if test:
        value += f" ({test.group(0).strip()})"
    frames, modules = parse_stack(crash_text)
    when = datetime.datetime.fromtimestamp(os.path.getmtime(game_log_path) if os.path.exists(game_log_path) else time.time(),
                                           datetime.timezone.utc)

    tags = {"kind": "crash", "reason": reason, "vr": vr}
    for k, v in (("gpu", gpu and gpu.group(1)), ("driver", driver and driver.group(1)), ("refresh_hz", hz and hz.group(1)),
                 ("eye_px", eye and eye.group(1)), ("camera", camera), ("kit", kit), ("test", "yes" if test else None)):
        if v:
            tags[k] = v.strip()[:200]

    lines = [
        "KartFrame crash report",
        f"What happened: {sentence}",
        f"When: {when:%Y-%m-%d %H:%M} UTC",
        f"KartFrame: {version} ({release}, {environment})" + (f", kit {kit}" if kit else ""),
        f"System: {osr.get('NAME', 'unknown')} {osr.get('VERSION_ID', '')} {osr.get('BUILD_ID', '')}".rstrip(),
        f"GPU: {tags.get('gpu', 'unknown')}, driver {tags.get('driver', 'unknown')}",
        f"VR: {vr}, refresh {tags.get('refresh_hz', '?')} Hz, eye size {tags.get('eye_px', '?')}, camera {camera or '?'}",
        f"Exit status: {a.status}",
    ]
    if crash_text:
        lines += ["", f"The game's crash file (crash_{reason}.txt):", crash_text.rstrip()]
    elif not frames:
        lines += ["", "The game left no crash file, so there is no stack."]
    report = scrub("\n".join(lines) + "\n")

    exception = {"type": exc_type, "value": value, "mechanism": {"type": "signalhandler" if exc_type.startswith("SIG") else "generic",
                                                                 "handled": False, "synthetic": True}}
    if frames:
        exception["stacktrace"] = {"frames": [{"instruction_addr": f"0x{pc:x}"} for pc in reversed(frames)]}
    images = [{"type": "elf", "code_id": m["build_id"], "debug_id": debug_id(m["build_id"]),
               "code_file": scrub(m["path"]) or "?", "image_addr": f"0x{m['start']:x}", "image_size": m["size"], "arch": "arm64"}
              for m in modules if m["build_id"]]
    event = {
        "timestamp": when.timestamp(),
        "platform": "native",
        "level": "fatal",
        "logger": "kartframe",
        "release": release,
        "environment": environment,
        "tags": tags,
        "contexts": {
            "os": {"name": osr.get("NAME", "Linux"), "version": osr.get("VERSION_ID", ""), "build": osr.get("BUILD_ID", "")},
            "gpu": {"name": tags.get("gpu", "unknown"), "version": tags.get("driver", "")},
            "app": {"app_name": "KartFrame", "app_version": version},
        },
        "exception": {"values": [exception]},
        "extra": {"exit_status": a.status},
        # Nothing Sentry could place a player by: no user, no server name, and no IP address taken from the request.
        "user": {"geo": {}},
        "sdk": {"name": "kartframe.crash-report", "version": "1", "settings": {"infer_ip": "never"}},
    }
    if images:
        event["debug_meta"] = {"images": images}
    event = json.loads(scrub(json.dumps(event)))

    files = {"report.txt": report, "game-log.txt": game_log, "launch-log.txt": launch_log}
    whats_sent = "".join(f"----- {name} -----\n{text}\n" for name, text in files.items())
    whats_sent += "----- the event Sentry indexes -----\n" + json.dumps(event, indent=1) + "\n"

    out = a.out
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        os.remove(os.path.join(out, name))
    for name, text in list(files.items()) + [("event.json", json.dumps(event)), ("whats-sent.txt", whats_sent),
                                             ("summary.txt", f"{sentence} {when:%Y-%m-%d %H:%M} UTC.\n")]:
        with open(os.path.join(out, name), "w", encoding="utf-8") as f:
            f.write(text)
    if find_dsn(a.app):
        open(os.path.join(out, "sendable"), "w").close()
    print(f"crash report prepared in {out}: {sentence} ({len(frames)} frames; "
          f"{'Send Report offered' if find_dsn(a.app) else 'no DSN: Send Report not offered'})")
    return 0


# ---- send -------------------------------------------------------------------------------------

def envelope(event, files, dsn):
    event = dict(event, event_id=uuid.uuid4().hex)
    sent_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
    parts = [json.dumps({"event_id": event["event_id"], "dsn": dsn, "sent_at": sent_at}).encode()]
    body = json.dumps(event).encode()
    parts += [json.dumps({"type": "event", "length": len(body)}).encode(), body]
    for name, data in files:
        parts += [json.dumps({"type": "attachment", "length": len(data), "filename": name,
                              "content_type": "text/plain"}).encode(), data]
    return event["event_id"], b"\n".join(parts) + b"\n"


def send(a):
    dsn = find_dsn(a.app)
    parsed = parse_dsn(dsn) if dsn else None
    if not parsed:
        print("crash report: no Sentry DSN, nothing sent")
        return 2
    url, key = parsed
    try:
        event = json.loads(read(os.path.join(a.dir, "event.json")))
    except ValueError:
        print(f"crash report: no readable event.json in {a.dir}, nothing sent")
        return 2
    files = []
    for name in ("report.txt", "game-log.txt", "launch-log.txt"):
        p = os.path.join(a.dir, name)
        if os.path.isfile(p):
            with open(p, "rb") as f:
                files.append((name, f.read()))
    event_id, body = envelope(event, files, dsn)
    req = urllib.request.Request(url, data=body, method="POST", headers={
        "Content-Type": "application/x-sentry-envelope",
        "User-Agent": "kartframe-crash-report/1",
        "X-Sentry-Auth": f"Sentry sentry_version=7, sentry_client=kartframe-crash-report/1, sentry_key={key}",
    })
    try:
        with urllib.request.urlopen(req, timeout=30, context=ssl.create_default_context()) as r:
            r.read()
            ok = 200 <= r.status < 300
    except Exception as e:  # noqa: BLE001 - every failure is reported the same way
        print(f"crash report: sending failed ({e.__class__.__name__}: {e})")
        return 1
    print(f"crash report sent: event {event_id}" if ok else "crash report: Sentry did not accept it")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--session", required=True)
    p.add_argument("--status", required=True, type=int)
    p.add_argument("--run-log", default="")
    p.add_argument("--app", default="")
    p.add_argument("--config", default="")
    p.add_argument("--out", required=True)
    s = sub.add_parser("send")
    s.add_argument("dir")
    s.add_argument("--app", default="")
    c = sub.add_parser("can-send")
    c.add_argument("--app", default="")
    a = ap.parse_args()
    if a.cmd == "prepare":
        return prepare(a)
    if a.cmd == "send":
        return send(a)
    return 0 if find_dsn(a.app) else 1


if __name__ == "__main__":
    sys.exit(main())
