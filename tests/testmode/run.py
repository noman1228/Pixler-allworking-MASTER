"""Compile and run Test Mode host regressions without touching the firmware.

Requires a C++17 compiler. Windows can use an installed Visual Studio Build Tools
toolchain; other systems can use CXX, c++, g++, or clang++.
"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
from extract_methods import method


ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio" / "testmode-host-tests"


def compiler_command(source, name, includes=()):
    compiler = os.environ.get("CXX") or next(
        (candidate for candidate in ("c++", "g++", "clang++") if shutil.which(candidate)), None
    )
    directories = [ROOT / "include", *includes]
    executable = BUILD / (name + (".exe" if os.name == "nt" else ""))
    if compiler:
        return [compiler, "-std=c++17", "-Wall", "-Wextra", "-pthread", *[f"-I{d}" for d in directories], str(source), "-o", str(executable)], executable

    command = ["cl", "/nologo", "/EHsc", "/std:c++17", "/W4", *[f"/I{d}" for d in directories], f"/Fo:{BUILD / (name + '.obj')}", f"/Fe:{executable}", str(source)]
    if shutil.which("cl"):
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
    pixel_source = (ROOT / "src/output/OutputPixel.cpp").read_text(encoding="utf-8-sig")
    (BUILD / "physical_methods.inc").write_text(
        method(pixel_source, "uint32_t c_OutputPixel::GetTestPixelCount()") +
        method(pixel_source, "void c_OutputPixel::WriteTestPixel("), encoding="utf-8"
    )
    (BUILD / "pixel_allocation_method.inc").write_text(
        method(pixel_source, "void c_OutputPixel::SetOutputBufferSize("), encoding="utf-8"
    )
    relay_source = (ROOT / "src/output/OutputRelay.cpp").read_text(encoding="utf-8-sig")
    (BUILD / "relay_methods.inc").write_text(
        method(relay_source, "void c_OutputRelay::OutputValue(") +
        method(relay_source, "void c_OutputRelay::SetTestModePaused("), encoding="utf-8"
    )
    output_source = (ROOT / "src/output/OutputMgr.cpp").read_text(encoding="utf-8-sig")
    input_source = (ROOT / "src/input/InputMgr.cpp").read_text(encoding="utf-8-sig")
    runtime = method(output_source, "const char* TestPatternName(")
    for name in ("SetTestMode", "GetTestModeStatus", "StopTestMode", "ServiceTestMode", "RenderTestMode",
                 "WriteChannelData", "ClearBuffer", "PauseOutputs", "UpdateDisplayBufferReferences "):
        result = "bool" if name == "SetTestMode" else "void"
        runtime += method(output_source, f"{result} c_OutputMgr::{name}(")
    for name in ("SetOperationalState", "SetTestModePaused", "ApplyOperationalState"):
        signature = f"void c_InputMgr::{name}"
        runtime += method(input_source, signature)
    (BUILD / "runtime_methods.inc").write_text(runtime, encoding="utf-8")
    web_source = (ROOT / "src/WebMgr.cpp").read_text(encoding="utf-8-sig")
    (BUILD / "api_route.inc").write_text(
        "void InstallTestApi() {\n" + method(web_source, 'webServer.on ("/api/testmode"') + ");\n}\n",
        encoding="utf-8"
    )
    json_header = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"), None)
    if json_header is None:
        raise RuntimeError("ArduinoJson dependency is missing; run a PlatformIO build first")
    for name in ("patterns", "physical_pixels", "relays", "runtime"):
        command, executable = compiler_command(ROOT / f"tests/testmode/{name}.cpp", name, [BUILD, json_header.parent])
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(executable)], cwd=ROOT, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
