"""Run the production frame coordinator against DMA and allocator fixtures."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/i2s-ownership-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    header = (ROOT / "include/output/OutputI2s.hpp").read_text(encoding="utf-8-sig")
    (BUILD / "coordinator_class.inc").write_text(helper.method(header, "class c_OutputI2s") + ";\n", encoding="utf-8")
    source = (ROOT / "src/output/OutputI2s.cpp").read_text(encoding="utf-8-sig")
    signatures = re.findall(r"(?:void|bool) c_OutputI2s::\w+\(", source)
    (BUILD / "coordinator_methods.inc").write_text("\n".join(helper.method(source, signature) for signature in signatures), encoding="utf-8")
    json = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
    command, executable = helper.compiler_command(ROOT / "tests/i2s/ownership.cpp", "ownership",
        [ROOT / "include", BUILD, json.parent])
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    main()
