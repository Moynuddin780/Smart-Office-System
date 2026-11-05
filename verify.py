#!/usr/bin/env python3
"""Smart Office — verification.

Checks the things that are worth checking and EXITS NON-ZERO when any of them
fails. A verifier that prints "failed" and exits 0 is worse than no verifier:
it makes a broken deployment look checked.

    ./verify.py            everything it can reach
    ./verify.py --quick    skip the checks that need the stack running
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parent
GRN, RED, YEL, DIM, OFF = "\033[32m", "\033[31m", "\033[33m", "\033[2m", "\033[0m"

results: list[tuple[str, bool, str]] = []


def check(name: str, fn) -> bool:
    try:
        ok, detail = fn()
    except Exception as exc:                       # noqa: BLE001
        ok, detail = False, f"{type(exc).__name__}: {exc}"
    results.append((name, ok, detail))
    mark = f"{GRN}ok{OFF}  " if ok else f"{RED}FAIL{OFF}"
    print(f"  {mark}  {name}")
    if detail:
        print(f"        {DIM}{detail}{OFF}")
    return ok


def sh(cmd: list[str], timeout: int = 60) -> tuple[int, str]:
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr).strip()


# ── hardware ────────────────────────────────────────────────────────────────

def npu_present():
    rc, out = sh(["lspci", "-d", "1ff4:"])
    return (rc == 0 and bool(out.strip())), out.splitlines()[0] if out.strip() else ""


def npu_cores():
    rc, out = sh(["dxrt-cli", "-s"])
    cores = [l.strip() for l in out.splitlines() if l.startswith("NPU ")]
    # Judged on the OUTPUT, never on the exit code: piping such a check through
    # tail or head silently substitutes the pipe's status.
    return (len(cores) >= 1), " | ".join(cores)


def npu_broker():
    missing = [p for p in ("/dev/dxrt0", "/tmp/dxrt_dynamic_ipc.sock",
                           "/dev/shm/dxrt_monitor_v2") if not os.path.exists(p)]
    return (not missing), ("missing: " + ", ".join(missing) if missing else
                           "device node, IPC socket and telemetry file all present")


# ── artefacts ───────────────────────────────────────────────────────────────

def models():
    want = ["yolo26s_pose.dxnn", "scrfd_10g.dxnn", "ArcFace_MobileFaceNet.dxnn"]
    missing = [m for m in want if not (ROOT / "models" / m).exists()]
    return (not missing), ("missing: " + ", ".join(missing) if missing else
                           f"{len(want)} models present")


def scripts_parse():
    bad = []
    for s in ("setup.sh", "run.sh"):
        rc, out = sh(["bash", "-n", str(ROOT / s)])
        if rc != 0:
            bad.append(f"{s}: {out}")
    return (not bad), "; ".join(bad) if bad else "setup.sh and run.sh parse"


def compose_valid():
    env = {**os.environ, "DB_PASSWORD": "x", "JWT_SECRET": "y" * 20}
    p = subprocess.run(["docker", "compose", "config"], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    return (p.returncode == 0), (p.stderr.strip().splitlines()[0]
                                 if p.returncode else "docker compose config parses")


def secrets_not_shipped():
    """The one check whose failure is a security problem rather than a bug."""
    example = (ROOT / ".env.example").read_text()
    bad = []
    for line in example.splitlines():
        if line.startswith(("DB_PASSWORD=", "JWT_SECRET=")) and line.split("=", 1)[1]:
            bad.append(line.split("=", 1)[0])
    return (not bad), ("the example ships a value for: " + ", ".join(bad) if bad
                       else "no secret has a default")


# ── the C++ suite ───────────────────────────────────────────────────────────

def unit_tests():
    exe = ROOT / "build" / "tests" / "so_tests"
    if not exe.exists():
        return False, "build/tests/so_tests not built — run: cmake -S . -B build && cmake --build build"
    p = subprocess.run([str(exe)], cwd=ROOT, capture_output=True, text=True, timeout=900)
    tail = [l for l in p.stdout.splitlines() if "test cases:" in l or "assertions:" in l]
    return (p.returncode == 0), " | ".join(t.strip() for t in tail)


def three_core_pooling():
    """The premise the whole architecture rests on, re-measured."""
    exe = ROOT / "build" / "tools" / "bench_pool"
    if not exe.exists():
        return False, "build/tools/bench_pool not built"
    p = subprocess.run([str(exe), "models/yolo26s_pose.dxnn", "6", "8"],
                       cwd=ROOT, capture_output=True, text=True, timeout=600)
    verdict = [l.strip() for l in p.stdout.splitlines()
               if "speedup" in l or "cores showing" in l or "RESULT" in l]
    return (p.returncode == 0), " | ".join(verdict)


# ── the running stack ───────────────────────────────────────────────────────

def _base_url() -> str:
    """The URL the stack is reachable on, from .env.

    The value is taken up to the first whitespace or '#'. A .env line carries an
    inline comment, and without stripping it the port became
    "8090        # what you open in a browser" - which urllib reads as a host
    plus a FRAGMENT, so every request silently went to "/" and came back 200
    from the SPA. Every check then "passed" a request it had never made, which
    is worse than not checking at all.
    """
    port = "8090"
    env = ROOT / ".env"
    if env.exists():
        for line in env.read_text().splitlines():
            line = line.strip()
            if line.startswith("HTTP_PORT="):
                raw = line.split("=", 1)[1]
                raw = raw.split("#", 1)[0].strip()
                if raw.isdigit():
                    port = raw
    return f"http://localhost:{port}"


def health():
    with urllib.request.urlopen(_base_url() + "/healthz", timeout=10) as r:
        body = json.loads(r.read())
    detail = (f"db={body.get('database')} npu={body.get('npu')} "
              f"engines={body.get('engines')} cameras={body.get('cameras_online')}"
              f"/{body.get('cameras_running')}")
    return bool(body.get("ok")), detail


def auth_required():
    """Every route except login and healthz must refuse an anonymous caller."""
    protected = ["/api/cameras", "/api/employees", "/api/attendance/daily",
                 "/api/desk/daily", "/api/events", "/api/settings",
                 "/api/admin/tenants", "/api/auth/me"]
    leaked = []
    for path in protected:
        try:
            with urllib.request.urlopen(_base_url() + path, timeout=8) as r:
                if r.status < 400:
                    leaked.append(f"{path} -> {r.status}")
        except urllib.error.HTTPError as e:
            if e.code not in (401, 403):
                leaked.append(f"{path} -> {e.code}")
        except Exception as exc:                    # noqa: BLE001
            leaked.append(f"{path} -> {type(exc).__name__}")
    return (not leaked), ("ANSWERED WITHOUT A TOKEN: " + ", ".join(leaked) if leaked
                          else f"all {len(protected)} protected routes returned 401/403")


def spa_served():
    with urllib.request.urlopen(_base_url() + "/", timeout=10) as r:
        body = r.read(2048).decode("utf-8", "replace")
    return ("<div id=\"root\"" in body or "<script" in body), f"HTTP {r.status}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true",
                    help="skip the checks that need the stack running")
    args = ap.parse_args()

    print("Smart Office — verification\n")

    print("Hardware")
    check("a DEEPX device is on the bus", npu_present)
    check("dxrt-cli reports live NPU cores", npu_cores)
    check("the container's route to the NPU exists", npu_broker)

    print("\nArtefacts")
    check("the three models are present", models)
    check("setup.sh and run.sh parse", scripts_parse)
    check("docker compose config is valid", compose_valid)
    check("no secret ships with a default", secrets_not_shipped)

    print("\nCode")
    check("the C++ test suite passes", unit_tests)
    check("three-core NPU pooling still works", three_core_pooling)

    if not args.quick:
        print(f"\nThe running stack  {DIM}({_base_url()}){OFF}")
        check("/healthz reports ok", health)
        check("every protected route refuses an anonymous caller", auth_required)
        check("the dashboard is served", spa_served)
    else:
        print(f"\n{DIM}skipping the running-stack checks (--quick){OFF}")

    failed = [n for n, ok, _ in results if not ok]
    print()
    if failed:
        print(f"{RED}RESULT: FAIL{OFF} — {len(failed)} of {len(results)} checks failed")
        for n in failed:
            print(f"  - {n}")
        return 1
    print(f"{GRN}RESULT: PASS{OFF} — {len(results)} checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
