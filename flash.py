#!/usr/bin/env python3
"""
Usage: python3 flash.py [--iradio] [--boot] <port> <firmware_file>
       python3 flash.py --dump-boot <port> <output_file>
"""

import sys, os, time, threading, math, hashlib, serial, serial.tools.list_ports

BOOT_DUMP_ADDR = 0x08000000
BOOT_DUMP_LEN  = 0x2800

SEND_CONNECT_DEFAULT = bytes([57, 51, 5, 16, 211])
SEND_END_DEFAULT     = bytes([57, 51, 5, 238, 177])
SEND_UPDATE_DEFAULT  = bytes([57, 51, 5, 85, 24])
SEND_CONNECT_IRADIO  = bytes([57, 51, 5, 16, 129])
SEND_END_IRADIO      = bytes([57, 51, 5, 238, 95])
SEND_UPDATE_IRADIO   = bytes([57, 51, 5, 85, 198])

HEX_SIZE     = 251904

def load_firmware(path: str) -> bytearray:
    path = os.path.expanduser(path)
    if not os.path.exists(path):
        sys.exit(f"File not found: {path}")

    if path.lower().endswith(".bin"):
        data = open(path, "rb").read()
        buf = bytearray(data)
        if len(buf) < HEX_SIZE:
            buf.extend(b'\xff' * (HEX_SIZE - len(buf)))
    else:  # Intel HEX
        buf = bytearray(b'\xff' * HEX_SIZE)
        ext_addr = 0
        for line in open(path, errors="ignore"):
            line = line.strip()
            if not line.startswith(":") or len(line) < 11:
                continue
            ln  = int(line[1:3], 16)
            adr = int(line[3:7], 16)
            rt  = int(line[7:9], 16)
            if rt == 0:
                base = ext_addr + adr - 0x08002800
                for i in range(ln):
                    pos = base + i
                    if 0 <= pos < HEX_SIZE:
                        buf[pos] = int(line[9+i*2:11+i*2], 16)
            elif rt == 4:
                ext_addr = int(line[9:13], 16) << 16
    return buf

def flash_size(buf: bytearray) -> int:
    """Возвращает размер данных в байтах, округлённый до 1024 (последний непустой блок)."""
    # Ищем последний не-0xFF байт
    end = len(buf)
    while end > 0 and buf[end - 1] == 0xFF:
        end -= 1
    if end == 0:
        return 0
    # Округляем до 1024
    return ((end + 1023) // 1024) * 1024


class Flasher:
    def __init__(self, port_name, firmware, use_iradio=False):
        self.port_name = port_name
        self.firmware  = firmware
        self.use_iradio = use_iradio
        self.total_blocks = HEX_SIZE // 1024

        self.send_connect = SEND_CONNECT_IRADIO if use_iradio else SEND_CONNECT_DEFAULT
        self.send_end     = SEND_END_IRADIO     if use_iradio else SEND_END_DEFAULT
        self.send_update  = SEND_UPDATE_IRADIO  if use_iradio else SEND_UPDATE_DEFAULT
        self.checksum_offset = 0 if use_iradio else 82

        self.sendbuf  = bytearray(2052)
        self.recvbuf  = bytearray(29)
        self.sendbuf[0] = 87

        self.step          = 0
        self.recvcnt       = 0
        self.sendcnt       = 0
        self.g_block       = 0
        self.flg_connect   = True
        self.waiting_ack   = False
        self.retry_count   = 0
        self.last_send_t   = 0.0
        self.max_retries   = 3
        self.timeout       = 3.0
        self.port: serial.Serial | None = None
        self._done         = False
        self._error        = None

    def _checksum(self, buf, length):
        return (sum(buf[:length - 1]) + self.checksum_offset) & 0xFF

    def _send_packet(self):
        self.sendbuf[1] = (self.sendcnt >> 8) & 0xFF
        self.sendbuf[2] =  self.sendcnt & 0xFF
        self.sendbuf[3:1027] = self.firmware[self.sendcnt:self.sendcnt + 1024]
        self.sendbuf[1027] = self._checksum(self.sendbuf, 1028)
        self.port.write(bytes(self.sendbuf[:1028]))
        self.last_send_t = time.time()
        self.waiting_ack = True
        self.retry_count = 0

    def _retry(self):
        if self.retry_count < self.max_retries:
            self.retry_count += 1
            self.sendcnt  -= 1024
            self.g_block  -= 1
            self.waiting_ack = False
            self._send_packet()
            self.sendcnt += 1024
        else:
            self._error = "Max retries exceeded"
            self.port.close()

    def _on_byte(self, b):
        if b == 6:  # ACK
            self.recvcnt    = 0
            self.waiting_ack = False
            self.retry_count = 0

            if self.step < 3:
                self.step += 1
                self.port.write(self.send_connect)
            elif self.step == 3:
                self.port.write(self.send_update)
                self.step = 4
            elif self.step == 4:
                self.g_block += 1
                self._print_progress()
                self._send_packet()
                self.sendcnt += 1024
                if self.sendcnt >= HEX_SIZE:
                    self.step = 5
                    self.port.write(self.send_end)
                    time.sleep(0.1)
                    self.port.close()
                    self._done = True

        elif b == 255:  # NAK
            if self.step == 4 and self.sendcnt > 0:
                self._retry()
            else:
                self._error = "NAK during handshake"
                self.port.close()

        elif b == 0:  # connected
            self.recvcnt     = 0
            self.flg_connect = True

    def _print_progress(self):
        pct  = self.g_block / self.total_blocks * 100
        done = int(pct / 2)
        bar  = "#" * done + "-" * (50 - done)
        print(f"\r  [{bar}] {pct:5.1f}%  {self.g_block}/{self.total_blocks}", end="", flush=True)

    def _read_loop(self):
        buf = bytearray(1)
        while self.port and self.port.is_open:
            if self.waiting_ack and time.time() - self.last_send_t > self.timeout:
                self._retry()
            try:
                n = self.port.readinto(buf)
            except Exception:
                time.sleep(0.001)
                continue
            if not n:
                continue
            b = buf[0]
            if self.recvcnt < len(self.recvbuf):
                self.recvbuf[self.recvcnt] = b
                self.recvcnt += 1
                if b == 0:
                    self.sendcnt     = 0
                    self.flg_connect = True
                    time.sleep(0.2)
                else:
                    self.flg_connect = False
                    if self.recvcnt == 1:
                        self._on_byte(b)

    def flash(self):
        self.port = serial.Serial(
            self.port_name, 115200,
            bytesize=8, parity='N', stopbits=1, timeout=0.1
        )
        self.step        = 1
        self.sendcnt     = 0
        self.flg_connect = True

        t = threading.Thread(target=self._read_loop, daemon=True)
        t.start()

        for _ in range(4):
            self.port.write(self.send_connect)
            time.sleep(0.2)
            if not self.flg_connect:
                break
            self.sendcnt = 0

        if self.flg_connect:
            self._error = "No response from device"
            self.port.close()

        if self._error:
            print(f"\nError: {self._error}", file=sys.stderr)
            sys.exit(1)

        print(f"Flashing {os.path.basename(sys.argv[-1])} → {self.port_name}")
        self._print_progress()

        while not self._done and self.port.is_open:
            time.sleep(0.05)

        if self._error:
            print(f"\nError: {self._error}", file=sys.stderr)
            sys.exit(1)

        print(f"\nDone.")


def _read_line(port, deadline):
    line = bytearray()
    while time.time() < deadline:
        b = port.read(1)
        if not b:
            continue
        if b == b"\n":
            return bytes(line).rstrip(b"\r")
        line += b
    raise TimeoutError("no response")


def _read_exact(port, count, deadline):
    data = bytearray()
    while len(data) < count and time.time() < deadline:
        chunk = port.read(count - len(data))
        if chunk:
            data += chunk
    if len(data) != count:
        raise TimeoutError(f"got {len(data)} of {count} bytes")
    return bytes(data)


def dump_boot(port_name, out_path):
    """dumpboot через UART-консоль работающей прошивки; ничего не стирает."""
    port = serial.Serial(port_name, 115200, bytesize=8, parity="N",
                         stopbits=1, timeout=0.1, write_timeout=1.0)
    try:
        port.reset_input_buffer()
        port.write(b"dumpboot\n")
        port.flush()

        line = _read_line(port, time.time() + 5.0)
        while not line.startswith(b"RT880BOOT"):
            if not (line.startswith(b"READY ") or line.startswith(b"PROF ")):
                raise RuntimeError(f"unexpected response: {line!r}")
            line = _read_line(port, time.time() + 5.0)
        parts = line.decode("ascii", "replace").split()
        if len(parts) != 4 or parts[0] != "RT880BOOT":
            raise RuntimeError(f"unexpected response: {line!r}")

        addr = int(parts[1], 16)
        length = int(parts[2], 16)
        expected = int(parts[3], 16)
        if addr != BOOT_DUMP_ADDR or length != BOOT_DUMP_LEN:
            raise RuntimeError(f"unexpected range: {addr:#x}+{length:#x}")

        data = _read_exact(port, length, time.time() + 5.0)
        actual = sum(data) & 0xFFFFFFFF
        if actual != expected:
            raise RuntimeError(f"checksum mismatch: {actual:08x} != {expected:08x}")

        with open(out_path, "wb") as f:
            f.write(data)
        print(f"Bootloader: {out_path} ({len(data)} bytes, "
              f"sha256 {hashlib.sha256(data).hexdigest()})")
    finally:
        port.close()


def reboot_to_bootloader(port_name, timeout=2.0):
    """Команда boot в UART-консоли приложения, затем ожидание загрузчика."""
    port = serial.Serial(port_name, 115200, bytesize=8, parity="N",
                         stopbits=1, timeout=0.1, write_timeout=1.0)
    try:
        port.reset_input_buffer()
        port.write(b"boot\n")
        port.flush()
        while True:
            line = _read_line(port, time.time() + timeout)
            if line == b"OK":
                break
            if not (line.startswith(b"READY ") or line.startswith(b"PROF ")):
                raise RuntimeError(f"unexpected response: {line!r}")
    finally:
        port.close()
    # Загрузчик инициализирует UART примерно через 200 мс после перехода.
    time.sleep(0.7)


def main():
    args = sys.argv[1:]
    use_iradio = "--iradio" in args
    if use_iradio:
        args.remove("--iradio")
    dump = "--dump-boot" in args
    if dump:
        args.remove("--dump-boot")
    boot = "--boot" in args
    if boot:
        args.remove("--boot")

    if len(args) != 2:
        print(f"Usage: {sys.argv[0]} [--iradio] [--boot] <port> <firmware>")
        print(f"       {sys.argv[0]} --dump-boot <port> <output_file>")
        ports = [p.device for p in serial.tools.list_ports.comports()]
        if ports:
            print("Ports:", ", ".join(sorted(ports)))
        sys.exit(1)

    port_name, fw_path = args

    if dump:
        try:
            dump_boot(port_name, fw_path)
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)
        return

    if boot:
        try:
            reboot_to_bootloader(port_name)
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)
    firmware = load_firmware(fw_path)
    Flasher(port_name, firmware, use_iradio).flash()


if __name__ == "__main__":
    main()
