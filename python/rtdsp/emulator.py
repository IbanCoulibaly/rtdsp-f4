"""Launch the firmware on QEMU's netduinoplus2 (STM32F405) and connect to it.

    USART1 (data link)  -> TCP socket, server=on,wait=on: QEMU holds the CPU
                           in reset until we connect, so no boot frame is lost.
    USART2 (console)    -> a log file (or stdio for `make run`).
"""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

from . import REPO_ROOT
from .device import Device

DEFAULT_ELF = REPO_ROOT / "build" / "fw" / "qemu" / "rtdsp.elf"


def find_qemu() -> str | None:
    return shutil.which("qemu-system-arm") or next(
        (p for p in ("/opt/homebrew/bin/qemu-system-arm", "/usr/local/bin/qemu-system-arm")
         if os.path.exists(p)), None)


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class QemuBoard:
    def __init__(self, elf: Path = DEFAULT_ELF, console_log: Path | None = None,
                 extra_args: list[str] | None = None):
        self.elf = Path(elf)
        self.console_log = Path(console_log or REPO_ROOT / "build" / "qemu_console.log")
        self.extra_args = extra_args or []
        self.proc: subprocess.Popen | None = None
        self.device: Device | None = None

    def start(self) -> Device:
        qemu = find_qemu()
        if qemu is None:
            raise FileNotFoundError("qemu-system-arm not found")
        if not self.elf.exists():
            raise FileNotFoundError(f"{self.elf} missing: run `make fw`")
        port = _free_port()
        self.console_log.parent.mkdir(parents=True, exist_ok=True)
        cmd = [qemu, "-M", "netduinoplus2", "-display", "none", "-monitor", "none",
               "-kernel", str(self.elf),
               "-serial", f"tcp:127.0.0.1:{port},server=on,wait=on",
               "-serial", f"file:{self.console_log}", *self.extra_args]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 10.0
        while True:
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
                break
            except OSError:
                if self.proc.poll() is not None or time.monotonic() > deadline:
                    out = self.proc.stdout.read().decode(errors="replace") if self.proc.stdout else ""
                    raise RuntimeError(f"QEMU did not start: {out}")
                time.sleep(0.05)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.device = Device(sock)
        return self.device

    def console(self) -> str:
        return self.console_log.read_text(errors="replace") if self.console_log.exists() else ""

    def stop(self) -> None:
        if self.device:
            self.device.close()
            self.device = None
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None

    def __enter__(self) -> Device:
        return self.start()

    def __exit__(self, *exc) -> None:
        self.stop()
