"""Test images for the launcher's importer: writes an Xbox disc image
(XDVDFS) of a folder, so the importer can be tried without a real disc.

    make_xiso.py <folder> <out.iso> [--redump]

--redump puts the file system 0x18300000 bytes in, where it sits on a full
disc image, instead of at the start (a trimmed xiso). Directories are
written as balanced binary trees ordered case-insensitively, like the
real thing, so the reader's tree walk is exercised both ways.
"""
import os
import struct
import sys

SECTOR = 2048
REDUMP_BASE = 0x18300000


def entry_size(name):
    return (14 + len(name) + 3) & ~3


class Dir:
    def __init__(self, path):
        self.path = path
        self.names = sorted(os.listdir(path), key=lambda n: n.upper())
        self.children = {}
        self.sector = 0
        self.size = 0


def build_tree(names):
    """Balanced tree: returns (root index, {index: (left, right)})."""
    links = {}

    def sub(lo, hi):
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        links[mid] = (sub(lo, mid), sub(mid + 1, hi))
        return mid
    return sub(0, len(names)), links


def layout_dir(names):
    """Offsets of each entry (by index) in pre-order from the root, never
    crossing a sector boundary; returns (offsets, table size)."""
    root, links = build_tree(names)
    order = []

    def walk(i):
        if i is None:
            return
        order.append(i)
        walk(links[i][0])
        walk(links[i][1])
    walk(root)
    offs, pos = {}, 0
    for i in order:
        sz = entry_size(names[i])
        if pos // SECTOR != (pos + sz - 1) // SECTOR:
            pos = (pos // SECTOR + 1) * SECTOR
        offs[i] = pos
        pos += sz
    return offs, links, root, pos


def main():
    src, out = sys.argv[1], sys.argv[2]
    base = REDUMP_BASE if '--redump' in sys.argv else 0
    files = []  # (path, sector, size)
    next_sector = [33]

    def alloc(nbytes):
        s = next_sector[0]
        next_sector[0] += max(1, (nbytes + SECTOR - 1) // SECTOR)
        return s

    tables = []  # (sector, bytes)

    def do_dir(path):
        names = sorted(os.listdir(path), key=lambda n: n.upper())
        if not names:
            return 0, 0
        offs, links, root, size = layout_dir(names)
        sector = alloc(size)
        info = []
        for n in names:
            p = os.path.join(path, n)
            if os.path.isdir(p):
                s, sz = do_dir(p)
                info.append((s, sz, 0x10))
            else:
                sz = os.path.getsize(p)
                s = alloc(sz)
                files.append((p, s, sz))
                info.append((s, sz, 0x20))
        table = bytearray(b'\xff' * (((size + SECTOR - 1) // SECTOR) * SECTOR))
        for i, n in enumerate(names):
            l, r = links[i]
            lo = offs[l] // 4 if l is not None else 0
            ro = offs[r] // 4 if r is not None else 0
            s, sz, attr = info[i]
            e = struct.pack('<HHIIBB', lo, ro, s, sz, attr, len(n)) + n.encode('ascii')
            e += b'\x00' * (entry_size(n) - len(e))
            table[offs[i]:offs[i] + len(e)] = e
        tables.append((sector, bytes(table)))
        return sector, len(table)

    root_sector, root_size = do_dir(src)
    with open(out, 'wb') as f:
        vd = b'MICROSOFT*XBOX*MEDIA' + struct.pack('<II', root_sector, root_size) + b'\x00' * 8
        vd += b'\x00' * (0x7EC - len(vd)) + b'MICROSOFT*XBOX*MEDIA'
        f.seek(base + 32 * SECTOR)
        f.write(vd)
        for sector, table in tables:
            f.seek(base + sector * SECTOR)
            f.write(table)
        for p, s, sz in files:
            f.seek(base + s * SECTOR)
            with open(p, 'rb') as g:
                while True:
                    b = g.read(8 << 20)
                    if not b:
                        break
                    f.write(b)
        end = base + next_sector[0] * SECTOR
        f.truncate(end)
    print('wrote', out, len(files), 'files', end, 'bytes')


if __name__ == '__main__':
    main()
