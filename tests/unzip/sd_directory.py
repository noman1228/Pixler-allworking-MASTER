"""Exercise the installed, unmodified SdFat against a sparse RAM block device."""
from pathlib import Path
import os
import shutil
import subprocess
import sys

import run as extraction_runner
from run_upload import method

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio" / "sd-directory-host-tests"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    candidates = sorted((ROOT / ".pio/libdeps").glob("*/SdFat/src/FsLib/FsFile.cpp"), key=lambda path: (path.parts[-5] != "jmt_esp32_dig_octa", str(path)))
    library = candidates[0] if candidates else None
    if library is None:
        raise RuntimeError("SdFat missing; run a PlatformIO build first")
    library = library.parents[1]
    compatibility = BUILD / "compatibility.h"
    compatibility.write_text("#pragma once\n#include <stdint.h>\nclass __FlashStringHelper;\ninline uint32_t millis() { return 100; }\n#ifdef _MSC_VER\n#define __attribute__(value)\n#pragma warning(disable:4291)\n#endif\n", encoding="utf-8")
    (BUILD / "sd_directory_methods.inc").write_text(method((ROOT / "src/FileMgr.cpp").read_text(encoding="utf-8-sig"), "bool c_FileMgr::OpenNextSdEntry("), encoding="utf-8")
    sources = [ROOT / "tests/unzip/sd_directory_host.cpp"]
    for folder in ("common", "FatLib", "ExFatLib", "FsLib"):
        sources.extend(sorted((library / folder).glob("*.cpp")))
    executable = BUILD / ("sd-directory-tests.exe" if os.name == "nt" else "sd-directory-tests")
    defines = ["ENABLE_ARDUINO_FEATURES=0", "ENABLE_ARDUINO_STRING=0", "ENABLE_ARDUINO_SERIAL=0", "USE_BLOCK_DEVICE_INTERFACE=1", "SPI_DRIVER_SELECT=3", "SDFAT_FILE_TYPE=3"]
    compiler = os.environ.get("CXX") or next((name for name in ("c++", "g++", "clang++") if shutil.which(name)), None)
    if compiler and Path(compiler).stem.lower() not in ("cl", "cl.exe"):
        command = [compiler, "-std=c++17", "-include", str(compatibility), *["-D" + flag for flag in defines], "-I" + str(library), "-I" + str(BUILD), *map(str, sources), "-o", str(executable)]
    else:
        command = ["cl", "/nologo", "/EHsc", "/std:c++17", "/Zc:__cplusplus", "/D_CRT_SECURE_NO_WARNINGS", "/FI" + str(compatibility), *["/D" + flag for flag in defines], "/I" + str(library), "/I" + str(BUILD), "/Fe:" + str(executable), *map(str, sources)]
    extraction_runner.BUILD = BUILD
    extraction_runner.toolchain([command])
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
