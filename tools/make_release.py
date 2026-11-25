"""Packs the release candidate of Tactical Cover (REFERENCE 24).

    xmake build TacticalCover && xmake build CoverSF_configcheck
    python -I tools/make_release.py 1.0.0

Writes build/release_<version>_candidate/:
  Tactical Cover <version>.zip        SFSE/Plugins/TacticalCover.dll + SFSE/Plugins/TacticalCover.ini (the public INI), for Mod Organizer and Vortex
  TacticalCover.pdb                   the debug symbols of that DLL (for crash dumps; not for players)
  TacticalCover.user-full-log.ini     the author's own INI: the full log for the video series (rename to TacticalCover.ini in his copy)
  compare_config.py, compare_config_output.txt   the configuration check and what it printed (RESULT must be PASS)
  SHA256SUMS.txt                      the sums of everything above and of the two files inside the zip
The README.txt is written by hand. Refuses when the configuration check fails or the DLL is older than a source file.
"""
import hashlib
import pathlib
import shutil
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "windows" / "x64" / "releasedbg"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else "1.0.0"
    out = ROOT / "build" / f"release_{version}_candidate"
    out.mkdir(parents=True, exist_ok=True)
    dll = BIN / "TacticalCover.dll"
    pdb = BIN / "TacticalCover.pdb"
    newest = max(p.stat().st_mtime for p in (ROOT / "src").rglob("*") if p.is_file())
    if dll.stat().st_mtime < newest:
        sys.exit("TacticalCover.dll is older than the sources: xmake build TacticalCover first")
    public = ROOT / "data" / "release" / "TacticalCover.ini"
    user = ROOT / "data" / "release" / "TacticalCover.user-full-log.ini"

    check = subprocess.run([sys.executable, "-I", str(ROOT / "tools" / "compare_config.py")], capture_output=True, text=True)
    (out / "compare_config_output.txt").write_text(check.stdout, encoding="utf-8")
    shutil.copy2(ROOT / "tools" / "compare_config.py", out / "compare_config.py")
    if check.returncode != 0:
        sys.exit("the configuration check FAILED: see compare_config_output.txt")

    zpath = out / f"Tactical Cover {version}.zip"
    dll_bytes = dll.read_bytes()
    ini_bytes = public.read_bytes()
    with zipfile.ZipFile(zpath, "w", compression=zipfile.ZIP_DEFLATED) as z:
        for name, data in (("SFSE/Plugins/TacticalCover.dll", dll_bytes), ("SFSE/Plugins/TacticalCover.ini", ini_bytes)):
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 25, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    shutil.copy2(pdb, out / "TacticalCover.pdb")
    shutil.copy2(user, out / "TacticalCover.user-full-log.ini")

    lines = []
    for name in (zpath.name, "TacticalCover.pdb", "TacticalCover.user-full-log.ini", "compare_config.py", "compare_config_output.txt"):
        lines.append(f"{sha((out / name).read_bytes())} *{name}")
    lines.append(f"{sha(dll_bytes)} *{zpath.name}:SFSE/Plugins/TacticalCover.dll")
    lines.append(f"{sha(ini_bytes)} *{zpath.name}:SFSE/Plugins/TacticalCover.ini")
    (out / "SHA256SUMS.txt").write_bytes(("\n".join(lines) + "\n").encode("ascii"))  # LF, so that sha256sum -c reads it
    print("\n".join(lines))
    print(f"written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
