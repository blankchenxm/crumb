"""
Wrapper that runs idf.py mcp-server and keeps stderr noise out of the MCP stream.

ESP-IDF emits useful diagnostics on stderr, but MCP clients expect a clean stdio
channel. On this machine PowerShell profile failures can leak into stderr and
break the client-side parser, so we log stderr to a file and only optionally
forward filtered lines back to the parent process.
"""
import os
import subprocess
import sys
import threading

LOG_FILE = r"C:\Users\blank\mcp_debug.log"

_POWERSHELL_NOISE = (
    "WindowsPowerShell\\profile.ps1",
    "Microsoft.PowerShell_profile.ps1",
    "PSSecurityException",
    "about_Execution_Policies",
    "无法加载文件",
)


def _should_forward_stderr(line: str) -> bool:
    if any(marker in line for marker in _POWERSHELL_NOISE):
        return False
    return os.environ.get("IDF_MCP_FORWARD_STDERR", "").lower() in {"1", "true", "yes"}


def tee_stderr(proc: subprocess.Popen[str], log_fh) -> None:
    assert proc.stderr is not None
    for line in proc.stderr:
        log_fh.write(line)
        log_fh.flush()
        if _should_forward_stderr(line):
            sys.stderr.write(line)
            sys.stderr.flush()


idf_py = os.path.join(os.environ.get("IDF_PATH", ""), "tools", "idf.py")
cmd = [sys.executable, "-u", idf_py, "mcp-server"]

env = os.environ.copy()
env["PYTHONUNBUFFERED"] = "1"

with open(LOG_FILE, "w", encoding="utf-8", errors="replace") as log_fh:
    log_fh.write("=== MCP server started ===\n")
    log_fh.flush()

    proc = subprocess.Popen(
        cmd,
        stdin=sys.stdin.buffer,
        stdout=sys.stdout.buffer,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        cwd=os.environ.get("IDF_MCP_WORKSPACE_FOLDER", os.getcwd()),
        env=env,
    )

    t = threading.Thread(target=tee_stderr, args=(proc, log_fh), daemon=True)
    t.start()

    proc.wait()
    t.join(timeout=5)
    log_fh.write(f"=== MCP server exited ({proc.returncode}) ===\n")
