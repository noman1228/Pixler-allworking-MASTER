"""Run production flash config upload callbacks against storage faults.

    python tests/config/run.py
"""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper

BUILD = ROOT / ".pio/config-host-tests"


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    source = (ROOT / "src/FileMgr.cpp").read_text(encoding="utf-8-sig")
    signatures = (
        "void c_FileMgr::DeleteFlashFile (",
        "bool c_FileMgr::RenameFlashFile (",
        "bool c_FileMgr::SaveFlashFile(const String & FileName, uint32_t index,",
    )
    (BUILD / "flash_methods.inc").write_text(
        "\n".join(helper.method(source, signature) for signature in signatures), encoding="utf-8"
    )
    source = (ROOT / "src/WebMgr.cpp").read_text(encoding="utf-8-sig")
    start = source.index('webServer.on ("/conf", HTTP_PUT')
    end = source.index("// Firmware upload handler", start)
    (BUILD / "config_route.inc").write_text(
        "void c_WebMgr::Install() {\n" + source[start:end] + "\n}\n", encoding="utf-8"
    )
    json = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"), None)
    if json is None:
        raise RuntimeError("ArduinoJson is missing; run a PlatformIO build first")
    for simulated in (False, True):
        name = "config-simulated" if simulated else "config-native"
        wrapper = BUILD / (name + ".cpp")
        wrapper.write_text(
            ("#define SIMULATE_SD\n" if simulated else "") +
            '#include "' + (ROOT / "tests/config/host.cpp").as_posix() + '"\n',
            encoding="utf-8",
        )
        command, executable = helper.compiler_command(
            wrapper, name, [BUILD, json.parent]
        )
        subprocess.run(command, cwd=BUILD, check=True)
        subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
