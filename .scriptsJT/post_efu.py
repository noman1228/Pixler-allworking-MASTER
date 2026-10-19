# post_efu.py - EFU post-build and packaging script
from SCons.Script import Import, AlwaysBuild
Import("env")

import os
import sys
import subprocess
from datetime import datetime
from pathlib import Path
from make_efu import make_efu
from colorama import Fore, Style, init as colorama_init
import shutil

# Optional: keep the helper around if you later autodetect boards/ports
try:
    import serial.tools.list_ports  # noqa: F401
except Exception:
    serial = None  # pyserial not installed; we'll degrade gracefully

# --- Color output on Windows too
colorama_init(autoreset=True)

# Paths and globals
project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
build_dir = Path(env.subst("$PROJECT_BUILD_DIR")).resolve()
output_dir = (project_dir / "firmware" / "EFU")
output_dir.mkdir(parents=True, exist_ok=True)
ENV_CACHE_FILE = project_dir / ".efu_env"


# -------------------------------------------
# Command helpers
# -------------------------------------------
def which(*candidates: str) -> str | None:
    """Return the first executable found in PATH among candidates."""
    for c in candidates:
        p = shutil.which(c)
        if p:
            return p
    return None


def run_cmd(args: list[str], cwd: Path | None = None) -> int:
    """Run a command (list form), echo it, and return exit code."""
    cmd_str = " ".join(args)
    print(f"{Fore.CYAN}$ {cmd_str}{Style.RESET_ALL}")
    try:
        res = subprocess.run(args, cwd=str(cwd) if cwd else None)
        return res.returncode
    except FileNotFoundError:
        print(f"{Fore.RED}✘ Command not found: {args[0]}{Style.RESET_ALL}")
        return 127


def run_gulp(cwd: Path) -> bool:
    """
    Try 'gulp', then 'npx gulp', then 'npm run gulp'.
    Returns True on success.
    """
    gulp = which("gulp")
    if gulp:
        if run_cmd([gulp], cwd=cwd) == 0:
            return True

    npx = which("npx")
    if npx:
        if run_cmd([npx, "gulp"], cwd=cwd) == 0:
            return True

    npm = which("npm")
    if npm:
        # assumes a "gulp" script exists in package.json
        if run_cmd([npm, "run", "gulp"], cwd=cwd) == 0:
            return True

    print(f"{Fore.RED}✘ Could not run gulp. Install it (npm i -D gulp-cli gulp) "
          f"or ensure package.json has a 'gulp' script.{Style.RESET_ALL}")
    return False


def pio_cmd() -> str:
    """Locate PlatformIO CLI ('pio' or 'platformio')."""
    return which("pio") or which("platformio") or "pio"


# -------------------------------------------
# Environment helpers
# -------------------------------------------
def get_cmd_env() -> str | None:
    # Respect -e / --environment on PlatformIO command line
    for i, arg in enumerate(sys.argv):
        if arg in {"-e", "--environment"} and i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return None


def save_cached_environment(env_name: str) -> None:
    try:
        ENV_CACHE_FILE.write_text(env_name, encoding="utf-8")
    except Exception:
        pass


def get_cached_environment() -> str | None:
    try:
        if ENV_CACHE_FILE.exists():
            return ENV_CACHE_FILE.read_text(encoding="utf-8").strip() or None
    except Exception:
        pass
    return None


def detect_latest_environment() -> str | None:
    try:
        candidates = [d for d in build_dir.iterdir() if d.is_dir()]
        if not candidates:
            return None
        return max(candidates, key=lambda d: d.stat().st_mtime).name
    except Exception:
        return None


def check_env_exists(env_name: str | None) -> bool:
    return bool(env_name) and (build_dir / env_name).is_dir()


def pick_environment() -> str | None:
    """
    Choose environment, favoring (in order):
    1) CLI -e
    2) Current $PIOENV if already built
    3) Cached previous selection
    4) Only one present in build dir
    5) Latest modified folder in build dir
    """
    from_env = env.subst("$PIOENV")
    cmd_env = get_cmd_env()

    # 1) CLI
    if check_env_exists(cmd_env):
        save_cached_environment(cmd_env)
        return cmd_env

    # 2) Current $PIOENV (if built)
    if check_env_exists(from_env):
        save_cached_environment(from_env)
        return from_env

    # Gather all envs in build dir
    all_envs = sorted([d.name for d in build_dir.iterdir() if d.is_dir()])

    # 3) Cached (if still present)
    cached = get_cached_environment()
    if cached and cached in all_envs:
        return cached

    # 4) Only one built
    if len(all_envs) == 1:
        save_cached_environment(all_envs[0])
        return all_envs[0]

    # 5) Latest modified
    latest = detect_latest_environment()
    if latest:
        save_cached_environment(latest)
        return latest

    return None


def short_git_sha(cwd: Path) -> str | None:
    git = which("git")
    if not git:
        return None
    try:
        out = subprocess.check_output([git, "rev-parse", "--short", "HEAD"], cwd=str(cwd))
        sha = out.decode("utf-8", errors="ignore").strip()
        return sha or None
    except Exception:
        return None


# -------------------------------------------
# Main logic
# -------------------------------------------
def after_build(target, source, env) -> None:
    # NOTE: SCons action signature is (target, source, env). Keep the exact arg name 'env'.
    print(f"{Fore.CYAN}====== EFU Builder ======{Style.RESET_ALL}")
    print(f"Build dir: {build_dir}")
    print(f"Output dir: {output_dir}")

    if not build_dir.exists():
        print(f"{Fore.RED}✘ Build directory does not exist: {build_dir}{Style.RESET_ALL}")
        return

    selected_env = pick_environment()
    if not selected_env or not check_env_exists(selected_env):
        print(f"{Fore.RED}✘ Could not detect a valid build environment in {build_dir}{Style.RESET_ALL}")
        return

    env_dir = build_dir / selected_env
    sketch_bin = env_dir / "firmware.bin"
    fs_bin = env_dir / "littlefs.bin"
    efu_out = env_dir / f"{selected_env}.efu"

    # Final filename: include date and optional git SHA
    date_tag = datetime.now().strftime("%Y%m%d")
    sha = short_git_sha(project_dir)
    suffix = f"_{sha}" if sha else ""
    final_filename = f"{selected_env}_{date_tag}{suffix}.efu"
    final_path = output_dir / final_filename

    # Ensure firmware exists; if missing, build it
    if not sketch_bin.exists():
        print(f"{Fore.YELLOW}⚠ firmware.bin missing. Building environment '{selected_env}'...{Style.RESET_ALL}")
        code = run_cmd([pio_cmd(), "run", "-e", selected_env], cwd=project_dir)
        if code != 0 or not sketch_bin.exists():
            print(f"{Fore.RED}✘ Firmware build failed or firmware.bin still missing.{Style.RESET_ALL}")
            return

    # Gulp (prep filesystem assets)
    print(f"{Fore.CYAN}⚙ Running gulp to prep filesystem...{Style.RESET_ALL}")
    if not run_gulp(project_dir):
        return

    # Build LittleFS
    print(f"{Fore.CYAN}📦 Building LittleFS image...{Style.RESET_ALL}")
    code = run_cmd([pio_cmd(), "run", "-t", "buildfs", "-e", selected_env], cwd=project_dir)
    if code != 0 or not fs_bin.exists():
        print(f"{Fore.RED}✘ Filesystem build failed or littlefs.bin not found.{Style.RESET_ALL}")
        return

    # Quick size sanity checks (catch zero/absurd builds early)
    try:
        fw_sz = sketch_bin.stat().st_size
        fs_sz = fs_bin.stat().st_size
    except Exception:
        print(f"{Fore.RED}✘ Unable to stat build artifacts.{Style.RESET_ALL}")
        return

    if fw_sz < 64 * 1024:
        print(f"{Fore.RED}✘ firmware.bin looks too small ({fw_sz} bytes). Aborting.{Style.RESET_ALL}")
        return
    if fs_sz < 4 * 1024:
        print(f"{Fore.RED}✘ littlefs.bin looks too small ({fs_sz} bytes). Aborting.{Style.RESET_ALL}")
        return

    print(f"{Fore.GREEN}✓ firmware.bin: {fw_sz:,} bytes{Style.RESET_ALL}")
    print(f"{Fore.GREEN}✓ littlefs.bin: {fs_sz:,} bytes{Style.RESET_ALL}")

    # Create EFU
    print(f"{Fore.CYAN}◼ Creating EFU: {efu_out}{Style.RESET_ALL}")
    try:
        make_efu(str(sketch_bin), str(fs_bin), str(efu_out))
    except Exception as ex:
        print(f"{Fore.RED}✘ EFU creation failed: {ex}{Style.RESET_ALL}")
        return

    # Validate EFU
    print(f"{Fore.CYAN}🔍 Validating EFU contents...{Style.RESET_ALL}")
    try:
        efu_size = efu_out.stat().st_size
    except Exception:
        print(f"{Fore.RED}✘ EFU output not found after creation.{Style.RESET_ALL}")
        return

    if efu_size < 4096:
        print(f"{Fore.RED}✘ EFU output too small to be valid ({efu_size} bytes).{Style.RESET_ALL}")
        return

    # Move to final destination (atomic where possible)
    try:
        os.replace(str(efu_out), str(final_path))
    except Exception as ex:
        print(f"{Fore.RED}✘ Failed to move EFU into {output_dir}: {ex}{Style.RESET_ALL}")
        return

    print(f"{Fore.GREEN}✓ EFU validated and saved to {final_path}{Style.RESET_ALL}")

    # Summary
    print("\n" + "=" * 60)
    print(f"{Fore.GREEN}EFU READY: {final_filename}{Style.RESET_ALL}")
    print("Upload Instructions:")
    print("1. Access your ESP32 device (e.g. http://esps-XXXX.local)")
    print("2. Go to the EFU upload section")
    print(f"3. Upload this file: {final_path}")
    print("=" * 60 + "\n")


# Hook to build system
AlwaysBuild(env.Alias("post_efu", "buildprog", after_build))
