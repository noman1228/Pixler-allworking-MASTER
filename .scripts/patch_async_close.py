"""Adapt the web-server fork to AsyncTCP's parameterless close API."""

from pathlib import Path

Import("env")

deps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env.subst("$PIOENV")
tcp_header = deps / "AsyncTCP" / "src" / "AsyncTCP.h"

# Older AsyncTCP releases still use the boolean argument. Only patch when
# the installed library explicitly identifies that overload as deprecated.
if tcp_header.is_file() and 'deprecated("Use AsyncClient::close() instead")' in tcp_header.read_text(encoding="utf-8"):
    for name in ("AsyncEventSource.cpp", "AsyncWebSocket.cpp"):
        source = deps / "ESPAsyncWebServer" / "src" / name
        if not source.is_file():
            continue
        original = source.read_bytes()
        patched = original.replace(b"->close(true)", b"->close()")
        if patched != original:
            source.write_bytes(patched)
            print("AsyncTCP compatibility: updated " + name)
