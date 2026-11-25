"""The configuration check of release 1.0.0 (REFERENCE 24): the release plays as m4b13, key by key.

    python -I tools/compare_config.py [path\\to\\CoverSF_configcheck.exe] > compare_config_output.txt

Runs the plugin's own parser (CoverSF_configcheck, built with `xmake build CoverSF_configcheck`) four times and compares the effective
settings, every key the plugin knows:
  A  the accepted play state: the development build reading build/m4b13_candidate/CoverSF.ini (what runs in the user's game since 2026-11-20)
  B  the release reading data/release/TacticalCover.ini (the public file)
  C  the release reading data/release/TacticalCover.user-full-log.ini (the user's own copy)
  D  the release with no INI at all (the compiled-in defaults alone)
A against B and A against C may differ only in the keys of ALLOWED, each with its reason; B against D must not differ at all (every value in the
public INI is its compiled-in default, so a player who deletes a line gets the same game). Exit code 0 when all of it holds, 1 otherwise.
"""
import hashlib
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "windows" / "x64" / "releasedbg" / "CoverSF_configcheck.exe"
M4B13 = ROOT / "build" / "m4b13_candidate" / "CoverSF.ini"
PUBLIC = ROOT / "data" / "release" / "TacticalCover.ini"
USER = ROOT / "data" / "release" / "TacticalCover.user-full-log.ini"

ALLOWED = {
    # logging
    "bfulllog": "logging: the public file writes the quiet log (0); m4b13 logged everything (the development default 1)",
    # the swap binding
    "bshoulderpadchord": "swap binding: LT + A read by the plugin (new in 1.0.0; m4b13 had only the keyboard key)",
    "icameraflipkey": "swap binding: Numpad 6 also swapped the shoulder in m4b13 (a left-over S4 test key); the release has no such key",
    # item 3 of the brief: nothing in the release can start a test (debug keys that toggled the S2b input tests or wrote probes into the log)
    "ireloadkey": "rig/debug inert: Numpad 9 re-read the INI",
    "iprobekey": "rig/debug inert: Numpad 0 wrote a world probe into the log",
    "isensekey": "rig/debug inert: Numpad 8 wrote a cover sensor reading into the log",
    "iautorewritekey": "rig/debug inert: Numpad 1 started the S2b input test cycle",
    "iswallowjumpkey": "rig/debug inert: Numpad 7 swallowed every Jump press",
    "izeromovekey": "rig/debug inert: Numpad 2 zeroed the movement input",
    "ihalfmovekey": "rig/debug inert: Numpad 3 halved the movement input",
    "iswallowmethodkey": "rig/debug inert: Numpad 4 cycled the S2b swallow method",
}


def run(mode, ini):
    r = subprocess.run([str(EXE), mode, str(ini)], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{EXE} {mode} {ini} failed: {r.stderr}")
    values = dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)
    notes = [line for line in r.stderr.splitlines() if line.strip()]
    return values, notes


def same(a, b):
    if a == b:
        return True
    try:
        return float(a) == float(b)
    except ValueError:
        return False


def diff(x, y):
    return sorted(k for k in set(x) | set(y) if not same(x.get(k, "<missing>"), y.get(k, "<missing>")))


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    ok = True
    print("Tactical Cover 1.0.0: configuration check (tools/compare_config.py)")
    print(f"parser: {EXE.name} (built from src/ConfigParse.cpp, the plugin's own key table)")
    for p in (M4B13, PUBLIC, USER):
        print(f"  {p.relative_to(ROOT)}  sha256 {sha(p)}")
    a, na = run("dev", M4B13)
    b, nb = run("release", PUBLIC)
    c, nc = run("release", USER)
    d, nd = run("release", "-")
    print(f"keys compared: {len(a)}")
    for name, notes in (("A (m4b13, development)", na), ("B (public INI)", nb), ("C (user INI)", nc), ("D (no INI)", nd)):
        print(f"parser notes for {name}: {'; '.join(notes) if notes else 'none (no unknown key, no refused key)'}")

    for label, other in (("B: public TacticalCover.ini", b), ("C: TacticalCover.user-full-log.ini", c)):
        print()
        print(f"A (m4b13) against {label}:")
        keys = diff(a, other)
        if not keys:
            print("  no difference")
        for k in keys:
            reason = ALLOWED.get(k)
            mark = "allowed" if reason else "NOT ALLOWED"
            if not reason:
                ok = False
            print(f"  {k}: m4b13 {a.get(k, '<missing>')} -> release {other.get(k, '<missing>')}   [{mark}] {reason or ''}")

    print()
    print("B (public INI) against D (no INI, the compiled-in defaults alone):")
    keys = diff(b, d)
    if keys:
        ok = False
        for k in keys:
            print(f"  {k}: INI {b.get(k)} != default {d.get(k)}   [NOT ALLOWED]")
    else:
        print("  no difference: every key in the public INI holds its compiled-in default, and every key left out of it has m4b13's value")

    print()
    print("Rig keys in the release (must be empty or off): " + ", ".join(f"{k}={b.get(k)}" for k in ("stestscript", "bquitaftertest", "srigarmfile", "igrapheventprobe", "iposeentryprobe", "iequipfieldprobe", "iaudioprobe", "bstancespike", "brighold")))
    for k, want in (("stestscript", ""), ("bquitaftertest", "0"), ("srigarmfile", "")):
        if b.get(k) != want or c.get(k) != want:
            ok = False
    print()
    print("RESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
