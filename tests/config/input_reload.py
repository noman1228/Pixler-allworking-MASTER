"""Run production input configuration reloads during an active FTP service poll."""

import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/input-reload-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "src/input/InputMgr.cpp",
                        help="Alternate production source for demonstrating a regression")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    source = args.source.read_text(encoding="utf-8-sig")
    (BUILD / "input_reload_method.inc").write_text(
        helper.method(source, "bool c_InputMgr::ProcessJsonConfig ("), encoding="utf-8"
    )
    json = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"), None)
    if json is None:
        raise RuntimeError("ArduinoJson is missing; run a PlatformIO build first")
    command, executable = helper.compiler_command(
        ROOT / "tests/config/input_reload.cpp", "input-reload",
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
