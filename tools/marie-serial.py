#!/usr/bin/env python3
"""Run shell commands on marie's busybox USB-serial console and print the output.

  marie-serial.py 'CMD' ['CMD' ...] [--tty /dev/ttyACM0] [--timeout S]
  marie-serial.py --props DTNODE ...    dump a device-tree node's properties

--props sets `stty -opost` on the phone's console so property bytes come
through unchanged, then decodes strings / u32 cells here.

For the ../marie busybox images (g_serial ACM, ttyGS0 on the phone).
"""
import argparse, re, sys, time
import serial


def run(port, cmd, timeout):
    port.reset_input_buffer()
    port.write(f'echo __B__; {cmd}; echo __E__\n'.encode())
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        buf += port.read(65536)
        m = re.search(rb'\n__B__\r?\n(.*?)\r?\n?__E__', buf, re.S)
        if m:
            return m.group(1).decode('utf-8', 'replace').replace('\r', '')
    sys.exit(f'timeout running: {cmd}\n(last output: {buf[-300:]!r})')


def raw(port, cmd, timeout):
    """Like run(), but returns the raw bytes between the markers."""
    port.reset_input_buffer()
    port.write(f'echo __B__; {cmd}; echo __E__\n'.encode())
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        buf += port.read(65536)
        i = buf.find(b'__B__\n')
        j = buf.rfind(b'__E__')
        if i >= 0 and j > i:
            return buf[i + 6:j]
    sys.exit(f'timeout running: {cmd}')


def show(name, val):
    printable = val and val[-1:] == b'\0' and all(32 <= c < 127 or c == 0 for c in val[:-1])
    if printable and (len(val) % 4 or 32 <= val[0] < 127):     # strings start printable; cells may not
        print(f'  {name} = ' + ' | '.join(repr(x.decode()) for x in val[:-1].split(b'\0')))
    elif val and len(val) % 4 == 0:
        print(f'  {name} = <' + ' '.join('%#x' % int.from_bytes(val[k:k + 4], 'big') for k in range(0, len(val), 4)) + '>')
    else:
        print(f'  {name} = [{val.hex()}]')


def props(port, node, timeout):
    print(node)
    out = raw(port, f'for f in {node}/*; do [ -f "$f" ] && {{ echo "@@P ${{f##*/}}"; cat "$f"; echo "@@Z"; }}; done', timeout)
    for part in out.split(b'@@P ')[1:]:
        name, _, rest = part.partition(b'\n')
        val = rest[:rest.rfind(b'@@Z')] if b'@@Z' in rest else rest   # cat adds no newline
        show(name.decode(errors='replace'), val)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmds', nargs='*')
    ap.add_argument('--props', nargs='*', default=[])
    ap.add_argument('--tty', default='/dev/ttyACM0')
    ap.add_argument('--timeout', type=float, default=30)
    a = ap.parse_args()
    port = serial.Serial(a.tty, 115200, timeout=0.2)
    port.write(b'\n')
    time.sleep(0.5)
    for c in a.cmds:
        sys.stdout.write(run(port, c, a.timeout) + '\n')
    if a.props:
        run(port, 'stty -opost', a.timeout)
        for n in a.props:
            props(port, n, a.timeout)


if __name__ == '__main__':
    main()
