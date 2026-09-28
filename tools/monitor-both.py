#!/usr/bin/env python3
"""Watch two DustGate boards on one screen, one clock — `bash dev.sh monitor both`.

WHY THIS EXISTS. Two `pio device monitor` windows show two logs with two sets of
timestamps nobody is printing, and a NodeLink fault is almost always a question
of ORDER: did the node see the connect before the primary gave up on it, did the
WELCOME go out before the socket was reset. Interleaving both boards' lines as
they arrive, stamped by this machine's one clock, answers that by reading down.

Everything is also written to a log file, so a whole session can be handed over
rather than scrolled back and pasted in pieces.

Typing: a line starting `p:` goes to the primary, `n:` to the node — the bench
commands (`mdnsprobe`, `probe`, ...) are typed at a board, and which board has
to be said. Anything else is refused rather than guessed.

Runs under PlatformIO's own Python (pyserial is already there). dev.sh resolves
the two ports from .dustgate-ports and passes them in; it is not meant to pick
boards itself.
"""
import os
import sys
import threading
import time
from datetime import datetime

import serial  # pyserial, shipped with PlatformIO

BAUD = 115200  # SERIAL_BAUD in firmware/config.h, monitor_speed in platformio.ini

COLOURS = {"P": "\033[36m", "N": "\033[33m"}  # cyan primary, yellow node
RESET = "\033[0m"

print_lock = threading.Lock()
stop = threading.Event()


def emit(tag, text, log):
    stamp = datetime.now().strftime("%H:%M:%S.%f")[:-3]
    line = f"{stamp} {tag} | {text}"
    with print_lock:
        colour = COLOURS.get(tag, "") if sys.stdout.isatty() else ""
        sys.stdout.write(f"{colour}{line}{RESET if colour else ''}\n")
        sys.stdout.flush()
        log.write(line + "\n")
        log.flush()


class Board:
    """One port, reopened whenever it comes back.

    The C5's USB serial comes straight off the chip, so the device node VANISHES
    on every reset and reappears a second later. A monitor that exits on the
    first disappearance misses the boot banner — which is usually the part that
    was wanted. So a vanished port is a state here, not an error.
    """

    def __init__(self, tag, port, log):
        self.tag, self.port, self.log = tag, port, log
        self.ser = None
        self.lock = threading.Lock()

    def _open(self):
        s = serial.Serial()
        s.port = self.port
        s.baudrate = BAUD
        s.timeout = 0.2
        # LOWER RTS FIRST, THEN DTR, AND ONLY AFTER OPEN — the ORDER is the fix.
        #
        # On the C5's USB-JTAG-serial the two lines ARE the reset circuit:
        # DTR=0 with RTS=1 holds the chip in reset (esptool's USBJTAGSerialReset
        # uses exactly that). macOS raises BOTH lines when the port opens, and
        # the first version of this set dtr=False, rts=False BEFORE open — which
        # pyserial applies DTR-first, passing straight through (DTR 0, RTS 1).
        # Every open reset the board. And since a reset drops the port, which
        # this class reopens, that became a loop: the primary rebooted every
        # 2.5 s for two minutes on 2026-09-27 (rst:0x15 USB_UART_HPSYS in every
        # banner) and the web UI went with it.
        #
        # Left at pyserial's defaults the port opens at (1,1), which is not a
        # reset; RTS down gives (1,0), DTR down gives (0,0). Neither step is.
        # EXCLUSIVE, like pio's own monitor. Without it macOS lets a second program
        # open the same port, and on 2026-09-27 that second program was esptool:
        # this monitor sat on the primary's port through a `dev.sh flash --ui`,
        # read the chip's replies before esptool could, and the flash died at 14%
        # with "The chip stopped responding" — half an erased filesystem. With
        # the lock, the flash is refused at the start instead (and dev.sh checks
        # the port before flashing anyway).
        s.exclusive = True
        s.open()
        # Tolerated, not required: a port with no modem lines (a pty in a test,
        # some adapters) refuses the ioctl, and watching it is still fine.
        for line in ("rts", "dtr"):
            try:
                setattr(s, line, False)
            except (serial.SerialException, OSError):
                pass
        return s

    def run(self):
        announced_gone = False
        buf = b""
        while not stop.is_set():
            if self.ser is None:
                if not os.path.exists(self.port):
                    if not announced_gone:
                        emit(self.tag, f"── {self.port} gone (reset or unplugged) — waiting", self.log)
                        announced_gone = True
                    time.sleep(0.25)
                    continue
                try:
                    with self.lock:
                        self.ser = self._open()
                    emit(self.tag, f"── connected to {self.port}", self.log)
                    announced_gone = False
                except (serial.SerialException, OSError) as e:
                    # "Resource busy" = another program holds it; dev.sh checks
                    # that up front, so here it is a race worth a retry.
                    emit(self.tag, f"── cannot open {self.port}: {e} — retrying", self.log)
                    time.sleep(1.0)
                    continue
            try:
                chunk = self.ser.read(512)
            except (serial.SerialException, OSError):
                with self.lock:
                    try:
                        self.ser.close()
                    except Exception:
                        pass
                    self.ser = None
                if buf:
                    emit(self.tag, buf.decode("utf-8", "replace").rstrip("\r"), self.log)
                    buf = b""
                # Do not reopen on the spot. An open is the one thing this tool
                # does that can disturb a board, so after an error give it time
                # to finish enumerating rather than knocking straight back in.
                time.sleep(1.0)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                emit(self.tag, raw.decode("utf-8", "replace").rstrip("\r"), self.log)

    def send(self, text):
        with self.lock:
            if self.ser is None:
                return False
            try:
                self.ser.write((text + "\n").encode())
                return True
            except (serial.SerialException, OSError):
                return False


def main():
    if len(sys.argv) != 4:
        print("usage: monitor-both.py <primary-port> <node-port> <log-file>", file=sys.stderr)
        return 2
    p_port, n_port, log_path = sys.argv[1:]
    os.makedirs(os.path.dirname(log_path) or ".", exist_ok=True)
    with open(log_path, "a", encoding="utf-8") as log:
        boards = {"p": Board("P", p_port, log), "n": Board("N", n_port, log)}
        emit("--", f"primary P = {p_port}   node N = {n_port}", log)
        emit("--", f"logging to {log_path}", log)
        emit("--", "type  p: <cmd>  or  n: <cmd>  to send to a board; Ctrl+C to quit", log)
        for b in boards.values():
            threading.Thread(target=b.run, daemon=True).start()
        try:
            for line in sys.stdin:
                line = line.rstrip("\n")
                if not line.strip():
                    continue
                who, sep, cmd = line.partition(":")
                b = boards.get(who.strip().lower()) if sep else None
                if b is None:
                    emit("--", "say which board:  p: <cmd>  or  n: <cmd>", log)
                    continue
                cmd = cmd.strip()
                if b.send(cmd):
                    emit(b.tag, f"<< {cmd}", log)
                else:
                    emit(b.tag, f"── not connected; '{cmd}' not sent", log)
            # stdin closed (not a terminal): keep watching until Ctrl+C.
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            emit("--", f"stopped — log is {log_path}", log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
