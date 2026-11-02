"""Compile the real pixel serializer and snapshot helpers with hardware stubs."""

from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/i2s-pixel-snapshot-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    source = (ROOT / "src/output/OutputPixel.cpp").read_text(encoding="utf-8-sig")
    signatures = (
        "uint64_t c_OutputPixel::GetWireFrameByteCount() const",
        "bool c_OutputPixel::SnapshotFrame(",
        "void c_OutputPixel::StartNewFrame ()",
        "void IRAM_ATTR c_OutputPixel::SetStartingSendPixelState()",
        "uint32_t IRAM_ATTR c_OutputPixel::GetIntensityData()",
        "uint32_t IRAM_ATTR c_OutputPixel::FramePrependData()",
        "uint32_t IRAM_ATTR c_OutputPixel::PixelPrependNulls()",
        "uint32_t IRAM_ATTR c_OutputPixel::PixelSendPrependIntensity()",
        "uint32_t IRAM_ATTR c_OutputPixel::PixelSendIntensity()",
        "uint32_t IRAM_ATTR c_OutputPixel::PixelAppendNulls()",
        "uint32_t IRAM_ATTR c_OutputPixel::FrameAppendData()",
        "uint32_t IRAM_ATTR c_OutputPixel::FrameDone()",
        "bool IRAM_ATTR c_OutputPixel::ISR_GetNextIntensityToSend (",
    )
    (BUILD / "pixel_snapshot_methods.inc").write_text(
        "\n".join(helper.method(source, signature) for signature in signatures), encoding="utf-8"
    )
    header = (ROOT / "include/output/OutputPixel.hpp").read_text(encoding="utf-8-sig")
    (BUILD / "pixel_snapshot_inline.inc").write_text(
        helper.method(header, "bool IRAM_ATTR ISR_MoreDataToSend"), encoding="utf-8"
    )
    command, executable = helper.compiler_command(
        ROOT / "tests/i2s/pixel_snapshot.cpp", "pixel-snapshot", [BUILD]
    )
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
