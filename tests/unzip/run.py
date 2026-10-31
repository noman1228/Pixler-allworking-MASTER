"""Exercise production cooperative extraction with real stored/deflated ZIPs.

Uses the project's installed unzipLIB and ArduinoJson dependencies. No board,
network connection, or PlatformIO download is needed. Run from any directory:

    python tests/unzip/run.py
"""

from io import BytesIO
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import zipfile


ROOT = Path(__file__).resolve().parents[2]
TESTS = ROOT / "tests" / "unzip"
BUILD = ROOT / ".pio" / "unzip-host-tests"


def fixtures():
    random_bytes = random.Random(271828).randbytes(180_000)
    payload = b"PSEQ" + random_bytes
    (BUILD / "payload.bin").write_bytes(payload)

    def archive(name, entries, compression=zipfile.ZIP_DEFLATED):
        output = BytesIO()
        with zipfile.ZipFile(output, "w", compression=compression) as stream:
            for entry, content in entries:
                stream.writestr(entry, content)
        result = output.getvalue()
        (BUILD / name).write_bytes(result)
        return bytearray(result)

    archive("same.xlz", [("same.fseq", payload)])
    archive("renamed.xlz", [("controller-17.fseq", payload)])
    archive("UPPER.XLZ", [("controller-17.fseq", payload)])
    archive("stored.xlz", [("controller-17.fseq", payload)], zipfile.ZIP_STORED)
    archive("nested.xlz", [("controllers/17/track.fseq", payload)])
    archive("multiple.xlz", [("first.fseq", payload), ("second.fseq", payload[::-1])])
    archive("upper-backup.zip", [("song.fseq.UNZIP.BAK", payload)])
    archive("fat-alias.zip", [("song.fseq.UNZIP.BAK ", payload)])
    archive("leading-space.zip", [(" .unzip.tmp", payload)])
    archive("batch.zip", [("first.fseq", payload), ("second.fseq", payload[::-1])])
    archive("traversal.zip", [("../outside.fseq", payload)])
    archive("nested.zip", [("folder/track.fseq", payload)])
    archive("directory.zip", [("folder/", b""), ("normal.fseq", payload)])
    nul_name = archive("nul-name.zip", [("songX.fseq", payload)])
    for signature, header_length, name_offset in ((b"PK\x03\x04", 30, 26), (b"PK\x01\x02", 46, 28)):
        header = nul_name.index(signature)
        assert struct.unpack_from("<H", nul_name, header + name_offset)[0] == len("songX.fseq")
        nul_name[header + header_length + 4] = 0
    (BUILD / "nul-name.zip").write_bytes(nul_name)
    crc_bad = archive("crc-bad.xlz", [("controller-17.fseq", payload)])
    local = crc_bad.index(b"PK\x03\x04")
    central = crc_bad.index(b"PK\x01\x02")
    crc = struct.unpack_from("<I", crc_bad, local + 14)[0] ^ 0x12345678
    struct.pack_into("<I", crc_bad, local + 14, crc)
    struct.pack_into("<I", crc_bad, central + 16, crc)
    (BUILD / "crc-bad.xlz").write_bytes(crc_bad)
    deflate_bad = archive("deflate-bad.xlz", [("controller-17.fseq", payload)])
    name_length, extra_length = struct.unpack_from("<HH", deflate_bad, 26)
    start = 30 + name_length + extra_length
    deflate_bad[start] = 7  # BFINAL=1 and the reserved BTYPE=3 -> inflate error.
    (BUILD / "deflate-bad.xlz").write_bytes(deflate_bad)
    (BUILD / "not-a-zip.xlz").write_bytes(b"incomplete upload")


def toolchain(commands):
    """Run commands in an existing compiler environment or initialize MSVC."""
    if os.name != "nt" or shutil.which("cl") or commands[0][0] != "cl":
        for command in commands:
            subprocess.run(command, cwd=BUILD, check=True)
        return
    installations = []
    for variable, fallback in (("ProgramFiles(x86)", "C:/Program Files (x86)"), ("ProgramFiles", "C:/Program Files")):
        installations.extend((Path(os.environ.get(variable, fallback)) / "Microsoft Visual Studio").glob("*/*/VC/Auxiliary/Build/vcvars64.bat"))
    if not installations:
        raise RuntimeError("No compiler found; set CXX or install Visual Studio Build Tools")
    script = BUILD / "build.cmd"
    lines = ["@echo off", "call " + subprocess.list2cmdline([str(sorted(installations)[-1])]) + " >nul"]
    for command in commands:
        lines.extend((subprocess.list2cmdline(command), "if errorlevel 1 exit /b %errorlevel%"))
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    subprocess.run(["cmd", "/c", str(script)], cwd=BUILD, check=True)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    fixtures()
    # Copy the unmodified production header so its quoted ESPixelStick.h include
    # resolves to our host seam, rather than to board hardware dependencies.
    shutil.copyfile(ROOT / "include" / "UnzipFiles.hpp", BUILD / "UnzipFiles.hpp")
    unzip_header = next((ROOT / ".pio" / "libdeps").glob("*/unzipLIB/src/unzipLIB.h"), None)
    json_header = next((ROOT / ".pio" / "libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"), None)
    if not unzip_header or not json_header:
        raise RuntimeError("unzipLIB/ArduinoJson missing; run a PlatformIO build first")
    library = unzip_header.parent
    include_paths = [BUILD, TESTS / "shims", library, json_header.parent]
    c_sources = [library / name for name in ("unzip.c", "adler32.c", "crc32.c", "inflate.c", "inffast.c", "inftrees.c", "zutil.c")]
    cpp_sources = [TESTS / "host.cpp", ROOT / "src" / "UnzipFiles.cpp", library / "unzipLIB.cpp"]
    executable = BUILD / ("unzip-tests.exe" if os.name == "nt" else "unzip-tests")
    compiler = os.environ.get("CXX") or next((name for name in ("c++", "g++", "clang++") if shutil.which(name)), None)
    if compiler and Path(compiler).stem.lower() not in ("cl", "cl.exe"):
        objects = []
        commands = []
        for source in c_sources:
            target = BUILD / (source.stem + ".o")
            objects.append(target)
            commands.append([compiler, "-x", "c", "-std=c99", "-D__MCUXPRESSO", *["-I" + str(path) for path in include_paths], "-c", str(source), "-o", str(target)])
        commands.append([compiler, "-std=c++17", "-DSUPPORT_UNZIP", "-DSUPPORT_OLED", "-D__MCUXPRESSO", *["-I" + str(path) for path in include_paths], *map(str, cpp_sources), *map(str, objects), "-o", str(executable)])
    else:
        commands = [["cl", "/nologo", "/EHsc", "/std:c++17", "/D_CRT_SECURE_NO_WARNINGS", "/DSUPPORT_UNZIP", "/DSUPPORT_OLED", "/D__MCUXPRESSO", *["/I" + str(path) for path in include_paths], "/Fe:" + str(executable), *map(str, cpp_sources + c_sources)]]
    toolchain(commands)
    subprocess.run([str(executable), str(BUILD)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
