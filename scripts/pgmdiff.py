#!/usr/bin/env python3
"""Сравнение двух наборов PGM (P5, 8 бит) из scripts/render-set.sh: средняя и наибольшая разница.
   scripts/pgmdiff.py DIR_A DIR_B"""
import os
import sys


def read_pgm(path):
    with open(path, 'rb') as f:
        data = f.read()
    parts, pos = [], 0
    while len(parts) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        parts.append(data[start:pos])
    assert parts[0] == b'P5' and parts[3] == b'255', path
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h]


def main(a, b):
    for root, _, files in sorted(os.walk(a)):
        for name in sorted(files):
            if not name.endswith('.pgm'):
                continue
            pa = os.path.join(root, name)
            pb = os.path.join(b, os.path.relpath(pa, a))
            wa, ha, da = read_pgm(pa)
            wb, hb, db = read_pgm(pb)
            rel = os.path.relpath(pa, a)
            if (wa, ha) != (wb, hb):
                print(f'{rel}: размер {wa}x{ha} / {wb}x{hb}')
                continue
            if da == db:
                continue
            diffs = [abs(x - y) for x, y in zip(da, db)]
            big = sum(1 for d in diffs if d > 16)
            print(f'{rel}: средняя {sum(diffs) / len(diffs):.2f}, наибольшая {max(diffs)}, '
                  f'пикселей с разницей > 16: {100 * big / len(diffs):.2f} %')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
