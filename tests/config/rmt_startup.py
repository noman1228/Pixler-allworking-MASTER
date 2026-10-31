"""Run production RMT startup and pause methods at controlled task boundaries."""

import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/rmt-startup-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", type=Path, default=ROOT / "include/output/OutputRmt.hpp",
                        help="Alternate production header for demonstrating a regression")
    args = parser.parse_args()
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    header = args.header.read_text(encoding="utf-8-sig")
    members = []
    for name in ("OutputIsPaused", "HasBeenInitialized"):
        declaration = re.search(r"\bbool\s+" + name + r"\s*=\s*(?:true|false)\s*;", header)
        if declaration is None:
            raise RuntimeError("Production member declaration missing: " + name)
        members.append(declaration.group())
    members.append(helper.method(header, "bool StartNextFrame"))
    (BUILD / "rmt_startup_members.inc").write_text("\n".join(members), encoding="utf-8")
    source = (ROOT / "src/output/OutputRmt.cpp").read_text(encoding="utf-8-sig")
    (BUILD / "rmt_startup_methods.inc").write_text(
        helper.method(source, "void c_OutputRmt::Begin (") +
        helper.method(source, "void c_OutputRmt::PauseOutput("), encoding="utf-8"
    )
    command, executable = helper.compiler_command(
        ROOT / "tests/config/rmt_startup.cpp", "rmt-startup", [BUILD]
    )
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
