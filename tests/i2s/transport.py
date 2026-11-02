"""Compile the production I2S transport against independent hardware seams."""

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/i2s-transport-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    # Real SDK register/descriptor bitfield layouts, including hardware widths.
    sdk = ROOT / ".pio/packages/framework-arduinoespressif32/tools/sdk/esp32/include"
    registers = sdk / "soc/esp32/include/soc/i2s_struct.h"
    descriptor = sdk / "esp_rom/include/esp32/rom/lldesc.h"
    text = descriptor.read_text(encoding="utf-8")
    start = text.index("typedef struct lldesc_s {")
    end = text.index("} lldesc_t;", start) + len("} lldesc_t;")
    (BUILD / "hardware_types.inc").write_text(
        registers.read_text(encoding="utf-8") + "\n" + text[start:end] + "\n", encoding="utf-8"
    )
    header = (ROOT / "include/output/ParallelI2sBus.hpp").read_text(encoding="utf-8-sig")
    source = (ROOT / "src/output/ParallelI2sBus.cpp").read_text(encoding="utf-8-sig")
    # Platform headers are replaced by the seams in transport.cpp. Every
    # production declaration and method remains present. The sole instruction
    # substitution models Xtensa's memory barrier on a non-Xtensa host.
    remove_includes = lambda value: re.sub(r'^\s*#include[^\n]*$', "", value, flags=re.MULTILINE)
    (BUILD / "transport_header.inc").write_text(remove_includes(header), encoding="utf-8")
    barrier = '__asm__ __volatile__("memw" ::: "memory");'
    if source.count(barrier) != 1:
        raise RuntimeError("Expected one Xtensa publication barrier")
    source = remove_includes(source).replace(barrier, "publication_barrier();")
    (BUILD / "transport_methods.inc").write_text(source, encoding="utf-8")
    command, executable = helper.compiler_command(
        ROOT / "tests/i2s/transport.cpp", "i2s-transport", [BUILD]
    )
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
