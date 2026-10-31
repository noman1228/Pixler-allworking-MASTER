"""Run production SD upload/buffering/replacement methods against storage faults.

    python tests/unzip/run_upload.py
"""

from pathlib import Path
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio" / "upload-host-tests"


def method(source: str, signature: str) -> str:
    """Extract a production method, ignoring braces in comments and literals."""
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    state = "code"
    index = opening
    while index < len(source):
        char = source[index]
        following = source[index:index + 2]
        if state == "line":
            if char == "\n":
                state = "code"
        elif state == "block":
            if following == "*/":
                state = "code"
                index += 1
        elif state in ('"', "'"):
            if char == "\\":
                index += 1
            elif char == state:
                state = "code"
        elif following == "//":
            state = "line"
            index += 1
        elif following == "/*":
            state = "block"
            index += 1
        elif char in ('"', "'"):
            state = char
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1] + "\n"
        index += 1
    raise ValueError(f"No closing brace for {signature}")


def compiler_command(source, name, includes=()):
    """Use an installed C++17 compiler, initializing MSVC when necessary."""
    compiler = os.environ.get("CXX") or next(
        (candidate for candidate in ("c++", "g++", "clang++") if shutil.which(candidate)), None
    )
    executable = BUILD / (name + (".exe" if os.name == "nt" else ""))
    if compiler and Path(compiler).stem.lower() != "cl":
        return [compiler, "-std=c++17", "-Wall", "-Wextra", "-pthread", *[f"-I{d}" for d in includes], str(source), "-o", str(executable)], executable

    command = [compiler or "cl", "/nologo", "/EHsc", "/std:c++17", "/W4", *[f"/I{d}" for d in includes], f"/Fo:{BUILD / (name + '.obj')}", f"/Fe:{executable}", str(source)]
    if shutil.which(command[0]):
        return command, executable
    candidates = []
    for variable, fallback in (("ProgramFiles(x86)", "C:/Program Files (x86)"), ("ProgramFiles", "C:/Program Files")):
        installation = Path(os.environ.get(variable, fallback)) / "Microsoft Visual Studio"
        candidates.extend(installation.glob("*/*/VC/Auxiliary/Build/vcvars64.bat"))
    if os.name != "nt" or not candidates:
        raise RuntimeError("No C++ compiler found; set CXX or run from a Visual Studio developer command prompt")
    vcvars = sorted(candidates)[-1]
    script = BUILD / (name + ".cmd")
    script.write_text("@echo off\ncall " + subprocess.list2cmdline([str(vcvars)]) + " >nul\n" + subprocess.list2cmdline(command) + "\n", encoding="utf-8")
    return ["cmd", "/c", str(script)], executable


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "src/FileMgr.cpp").read_text(encoding="utf-8-sig")
    signatures = (
        "uint64_t c_FileMgr::WriteSdFileBuf (",
        "bool c_FileMgr::FlushSdFileBuffer(",
        "bool c_FileMgr::handleFileUpload (",
        "void c_FileMgr::handleFileUploadNewFile (",
        "void c_FileMgr::AbortSdFileUpload()",
        "bool c_FileMgr::IsUploadInProgress()",
        "bool c_FileMgr::IsCompressed(",
        "bool c_FileMgr::ReplaceSdFile(",
        "void c_FileMgr::GetSdInfo(",
    )
    (BUILD / "upload_methods.inc").write_text("\n".join(method(source, signature) for signature in signatures), encoding="utf-8")
    fpp = (ROOT / "src/service/FPPDiscovery.cpp").read_text(encoding="utf-8-sig")
    (BUILD / "fpp_upload_methods.inc").write_text(
        method(fpp, "void c_FPPDiscovery::ProcessFile (") +
        method(fpp, "void c_FPPDiscovery::FinishFileUpload(") +
        method(fpp, "void c_FPPDiscovery::ProcessGET (") +
        method(fpp, "void c_FPPDiscovery::ProcessPOST (") +
        method(fpp, "void c_FPPDiscovery::ProcessBody ("), encoding="utf-8")
    json = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"), None)
    if json is None:
        raise RuntimeError("ArduinoJson is missing; run a PlatformIO build first")
    command, executable = compiler_command(
        ROOT / "tests/unzip/upload_host.cpp", "upload-tests",
        [BUILD, ROOT / "tests/unzip/shims", json.parent],
    )
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
