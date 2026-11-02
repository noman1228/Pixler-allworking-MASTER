"""Compile and independently decode the portable parallel-I2S sample encoder."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/i2s-encoder-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    command, executable = helper.compiler_command(
        ROOT / "tests/i2s/encoder.cpp", "parallel-i2s-encoder", [ROOT / "include"]
    )
    # GCC/Clang can verify the firmware's C++11 compatibility directly. MSVC
    # starts at /std:c++14; the encoder itself uses only C++11 constructs.
    command = ["-std=c++11" if argument == "-std=c++17" else argument for argument in command]
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
