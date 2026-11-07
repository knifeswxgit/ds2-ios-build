#!/usr/bin/env python3
"""Decode logs/ds2_llamadas.bin (cvar ds2_func_counts) and compare two time ranges.

  ds2_func_counts.py FILE                       list the windows (t = watchdog seconds, functions, calls)
  ds2_func_counts.py FILE A1-A2 B1-B2 [N]       functions per second in range A vs range B:
      - called in B, never in A (code that runs only in B - e.g. a hang)
      - called in A, never in B
      - the biggest rate changes, and the hottest functions of B
Times are the watchdog seconds of the windows (the main log has "[llamadas] t=..." lines with the clock time).
"""
import struct, sys, collections

def windows(path):
    data = open(path, 'rb').read()
    off = 0
    while off + 12 <= len(data):
        magic, t, n = struct.unpack_from('<III', data, off)
        off += 12
        if magic != 0x31574E44:
            raise SystemExit('bad window header at %d' % (off - 12))
        pairs = struct.unpack_from('<%dI' % (2 * n), data, off)
        off += 8 * n
        yield t, dict(zip(pairs[0::2], pairs[1::2]))

def main():
    ws = list(windows(sys.argv[1]))
    if len(sys.argv) < 4:
        prev = 0
        for t, d in ws:
            print('t=%5d s  %6d functions  %8.1f M calls' % (t, len(d), sum(d.values()) / 1e6))
        return
    def rng(r):
        a, b = map(int, r.split('-'))
        acc = collections.Counter(); secs = 0; last = None
        for i, (t, d) in enumerate(ws):
            if a <= t <= b:
                acc.update(d)
                secs += t - (ws[i - 1][0] if i else 0)
        return acc, max(1, secs)
    A, sa = rng(sys.argv[2]); B, sb = rng(sys.argv[3])
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 60
    ra = {k: v / sa for k, v in A.items()}; rb = {k: v / sb for k, v in B.items()}
    def show(title, items):
        print('== ' + title)
        for k, a, b in items[:n]:
            print('  sub_%08X  A %10.1f/s  B %10.1f/s' % (k, a, b))
    only_b = sorted(((k, 0.0, v) for k, v in rb.items() if k not in ra), key=lambda x: -x[2])
    only_a = sorted(((k, v, 0.0) for k, v in ra.items() if k not in rb), key=lambda x: -x[1])
    both = [(k, ra[k], rb[k]) for k in ra if k in rb]
    up = sorted(both, key=lambda x: -(x[2] + 1) / (x[1] + 1))
    hot = sorted(((k, ra.get(k, 0.0), v) for k, v in rb.items()), key=lambda x: -x[2])
    print('A = %s (%d s, %d functions)  B = %s (%d s, %d functions)' % (sys.argv[2], sa, len(A), sys.argv[3], sb, len(B)))
    show('only in B', only_b)
    show('only in A', only_a)
    show('rate up in B (B/A)', up)
    show('hottest in B', hot)

main()
