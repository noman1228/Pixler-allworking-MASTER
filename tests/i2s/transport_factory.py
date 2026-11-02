"""Compile the production output factory in normal and parallel-I2S builds."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/i2s-factory-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    source = (ROOT / "src/output/OutputMgr.cpp").read_text(encoding="utf-8-sig")
    macro = source[source.index("#define AllocatePort("):source.index("// Local Data definitions")]
    (BUILD / "factory_method.inc").write_text(
        macro + helper.method(source, "void c_OutputMgr::InstantiateNewOutputChannel("), encoding="utf-8"
    )
    json = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
    for parallel in (False, True):
        name = "factory-i2s" if parallel else "factory-rmt"
        wrapper = BUILD / (name + ".cpp")
        wrapper.write_text(("#define SUPPORT_PARALLEL_I2S\n" if parallel else "") +
            '#include "' + (ROOT / "tests/i2s/transport_factory.cpp").as_posix() + '"\n', encoding="utf-8")
        command, executable = helper.compiler_command(wrapper, name, [BUILD, json.parent])
        subprocess.run(command, cwd=BUILD, check=True)
        subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    main()
