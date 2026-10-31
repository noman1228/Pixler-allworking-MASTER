"""Exercise the production config save, schedule, poll and load sequence."""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".pio/config-live-host-tests"
sys.dont_write_bytecode = True
sys.path.insert(0, str(ROOT / "tests/unzip"))
import run_upload as helper


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    helper.BUILD = BUILD
    source = (ROOT / "src/FileMgr.cpp").read_text(encoding="utf-8-sig")
    (BUILD / "flash_methods.inc").write_text("\n".join(helper.method(source, signature) for signature in (
        "void c_FileMgr::DeleteFlashFile (", "bool c_FileMgr::RenameFlashFile (",
        "bool c_FileMgr::SaveFlashFile(const String & FileName, uint32_t index,",
    )), encoding="utf-8")
    runtime = helper.method(source, "bool c_FileMgr::LoadFlashFile (")
    for owner, path, names in (
        ("c_InputMgr", "input/InputMgr.cpp", ("Process", "LoadConfig", "PollConfig")),
        ("c_OutputMgr", "output/OutputMgr.cpp", ("Poll", "LoadConfig", "ServiceTestMode")),
    ):
        source = (ROOT / "src" / path).read_text(encoding="utf-8-sig")
        for name in names:
            signature = re.search(r"void\s+" + owner + "::" + name + r"\s*\(", source).group()
            runtime += helper.method(source, signature)
        header = (ROOT / "include" / path.replace(".cpp", ".hpp")).read_text(encoding="utf-8-sig")
        schedule = re.search(r"void\s+ScheduleLoadConfig\s*\(\s*\)\s*\{[^}]*\}", header).group()
        runtime += re.sub(r"void\s+ScheduleLoadConfig", "void " + owner + "::ScheduleLoadConfig", schedule) + "\n"
    source = (ROOT / "src/main.cpp").read_text(encoding="utf-8-sig")
    runtime += helper.method(source, "void ScheduleLoadConfig() ").replace("ConfigReload.Schedule", "coreReload.Schedule")
    (BUILD / "live_methods.inc").write_text(runtime, encoding="utf-8")
    source = (ROOT / "src/WebMgr.cpp").read_text(encoding="utf-8-sig")
    start = source.index('webServer.on ("/conf", HTTP_PUT')
    end = source.index("// Firmware upload handler", start)
    (BUILD / "config_route.inc").write_text("void c_WebMgr::Install() {\n" + source[start:end] + "\n}\n", encoding="utf-8")
    (BUILD / "request_reload_method.inc").write_text(helper.method(source, "bool c_WebMgr::RequestReadConfigFile("), encoding="utf-8")
    json = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
    command, executable = helper.compiler_command(ROOT / "tests/config/live_reload.cpp", "live-reload", [ROOT / "include", ROOT / "tests/config", BUILD, json.parent])
    subprocess.run(command, cwd=BUILD, check=True)
    subprocess.run([str(executable)], cwd=BUILD, check=True)


if __name__ == "__main__":
    main()
