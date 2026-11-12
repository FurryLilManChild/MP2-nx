#!/usr/bin/env python3
"""Merge SWDK DKSH cache files (switch/deko/mpl/dksh_file.h, version 2) into one starter file.

usage: merge_dksh.py OUT BASE [MORE...]
Records with a bad CRC are dropped; a shader is kept once per (stage, GLSL hash), a module once per module hash,
a named stage once per name (the first file wins). Every input must carry BASE's compiler id."""
import struct
import sys
import zlib


def records(path):
    d = open(path, 'rb').read()
    if d[:4] != b'SWDK':
        raise SystemExit(f'{path}: no SWDK magic')
    version, cid = struct.unpack_from('<IQ', d, 4)
    if version != 2:
        raise SystemExit(f'{path}: SWDK version {version}, this tool reads 2')
    i, out, bad = 16, [], 0
    while i + 9 <= len(d):
        kind, size, crc = struct.unpack_from('<BII', d, i)
        p = d[i + 9:i + 9 + size]
        if len(p) < size:
            break  # a cut record ends the file
        if zlib.crc32(p, zlib.crc32(bytes([kind]))) & 0xffffffff != crc:
            bad += 1
        else:
            out.append((kind, p, d[i:i + 9 + size]))
        i += 9 + size
    return d[:16], cid, out, bad


def key(kind, p):
    if kind == 1:
        return (1, p[0], p[1:9])
    if kind == 2:
        return (2, p[:8])
    if kind == 3:
        n = p[1]
        return (3, p[2:2 + n])
    return None


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    out_path, inputs = sys.argv[1], sys.argv[2:]
    header, base_id, seen, body = None, None, set(), bytearray()
    for path in inputs:
        h, cid, recs, bad = records(path)
        if header is None:
            header, base_id = h, cid
        elif cid != base_id:
            print(f'{path}: compiler id {cid:#x}, not {base_id:#x}: skipped')
            continue
        added = 0
        for kind, p, raw in recs:
            k = key(kind, p)
            if k is None or k in seen:
                continue
            seen.add(k)
            body += raw
            added += 1
        print(f'{path}: {len(recs)} records, {bad} bad CRC, {added} added')
    open(out_path, 'wb').write(header + body)
    counts = {1: 0, 2: 0, 3: 0}
    for k in seen:
        counts[k[0]] += 1
    print(f'{out_path}: {counts[1]} shaders, {counts[2]} modules, {counts[3]} named stages, {len(header) + len(body)} bytes')


if __name__ == '__main__':
    main()
