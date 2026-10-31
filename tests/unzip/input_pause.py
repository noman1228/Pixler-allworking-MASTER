"""Compile actual input pause/process and FPP teardown methods on the host."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio" / "unzip-input-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_upload as host_runner

method = host_runner.method


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    input_source = (ROOT / "src/input/InputMgr.cpp").read_text(encoding="utf-8-sig")
    signatures = (
        "void c_InputMgr::SetFileExtractionPaused(",
        "void c_InputMgr::SetOperationalState (",
        "void c_InputMgr::SetTestModePaused(",
        "void c_InputMgr::ApplyOperationalState()",
        "void c_InputMgr::Process ()",
        "void c_InputMgr::ProcessButtonActions (",
        "void c_InputMgr::LoadConfig ()",
    )
    (BUILD / "input_methods.inc").write_text("\n".join(method(input_source, name) for name in signatures), encoding="utf-8")
    remote = (ROOT / "src/input/InputFPPRemote.cpp").read_text(encoding="utf-8-sig")
    macro_start = remote.index("#define DeAllocatePlayer()")
    macro_end = remote.index("//-----------------------------------------------------------------------------", macro_start)
    source = remote[macro_start:macro_end]
    for signature in ("void c_InputFPPRemote::StopPlaying ()", "bool c_InputFPPRemote::PlayingFile ()", "void c_InputFPPRemote::FppStopRemoteFilePlay ()"):
        source += method(remote, signature)
    file_player = (ROOT / "src/input/InputFPPRemotePlayFile.cpp").read_text(encoding="utf-8-sig")
    for signature in ("c_InputFPPRemotePlayFile::~c_InputFPPRemotePlayFile ()", "void c_InputFPPRemotePlayFile::Stop ()", "bool c_InputFPPRemotePlayFile::Poll ()"):
        source += method(file_player, signature)
    playlist = (ROOT / "src/input/InputFPPRemotePlayListFsm.cpp").read_text(encoding="utf-8-sig")
    source += method(playlist, "void fsm_PlayList_state_PlayingFile::Stop (void)")
    (BUILD / "player_methods.inc").write_text(source, encoding="utf-8")
    json_header = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"), None)
    if json_header is None:
        raise RuntimeError("ArduinoJson dependency missing; run PlatformIO build first")
    host_runner.BUILD = BUILD
    command, executable = host_runner.compiler_command(ROOT / "tests/unzip/input_pause.cpp", "input-pause-tests", [BUILD, json_header.parent])
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
