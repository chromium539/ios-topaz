#!/usr/bin/env python3
"""Pull the SWEET persistent console log (xnu 0047) off marie's userdata.

  marie-log-pull.py OUT.txt [--tty /dev/ttyACM0]

The phone runs a Linux with a busybox shell on USB serial (../marie
marie-busybox-respawn-delay15-v22.img: g_serial ACM, ttyGS0). READ-ONLY:
dd of the last 1 + 4 MiB/512 sectors of mmcblk0p70 (userdata), gzip|base64
over the console, decoded here. On marie the device block is 512 bytes
(xnu 0205), so the header is sector 0 and the ring starts at byte 512.
"""
import argparse, base64, gzip, re, struct, sys, time
import serial

LOG_RING = 4 << 20
BLOCK = 512
AREA = 1 + LOG_RING // BLOCK


def run(port, cmd, timeout):
    """Run cmd in the shell; return everything between our markers."""
    port.reset_input_buffer()
    port.write(f'echo __B__; {cmd}; echo __E__\n'.encode())
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        buf += port.read(65536)
        m = re.search(rb'\n__B__\r?\n(.*?)\r?\n__E__', buf, re.S)
        if m:
            return m.group(1).decode('ascii', 'replace')
    sys.exit(f'timeout running: {cmd}\n(last output: {buf[-300:]!r})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--tty', default='/dev/ttyACM0')
    ap.add_argument('--dev', default='mmcblk0p70')
    a = ap.parse_args()

    port = serial.Serial(a.tty, 115200, timeout=0.2)
    port.write(b'\n')
    time.sleep(0.5)
    sectors = int(run(port, f'cat /sys/class/block/{a.dev}/size', 10).strip().splitlines()[-1])
    skip = sectors - AREA
    print(f'{a.dev}: {sectors} sectors, log area at {skip}', file=sys.stderr)
    b64 = run(port, f'dd if=/dev/{a.dev} bs={BLOCK} skip={skip} count={AREA} 2>/dev/null | gzip -c | base64', 600)
    area = gzip.decompress(base64.b64decode(''.join(b64.split())))
    open(a.out + '.bin', 'wb').write(area)

    magic, version, size, total, seq = struct.unpack_from('<8sIIQQ', area, 0)
    if magic != b'SWEETLOG':
        sys.exit('no SWEETLOG header (log never flushed, or wrong area)')
    ring = area[BLOCK:BLOCK + size]
    text = ring[:total] if total <= size else ring[total % size:] + ring[:total % size]
    open(a.out, 'w').write(text.decode('utf-8', 'replace').replace('\r', ''))
    print(f'version {version}, ring {size}, total {total} bytes, flush #{seq}'
          f'{" (wrapped)" if total > size else ""} -> {a.out}', file=sys.stderr)


if __name__ == '__main__':
    main()
