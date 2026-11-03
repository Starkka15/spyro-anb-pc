# Unpacks an Xbox 360 "Xbox Original" GOD package: strips the SVOD hash blocks, then extracts the GDF filesystem.
# usage: god_unpack.py <package .data dir> <output dir>
import os, struct, sys

BLOCK = 0x1000
BLOCKS_PER_GROUP = 0xCC
GROUPS_PER_FILE = 0xCB
SECTOR = 0x800


class Image:
    """Reads the logical image through the hash blocks of the Data#### files."""
    def __init__(self, data_dir):
        self.files = [os.path.join(data_dir, f) for f in sorted(os.listdir(data_dir)) if f.startswith('Data')]
        self.handles = [open(f, 'rb') for f in self.files]
        self.blocks_per_file = BLOCKS_PER_GROUP * GROUPS_PER_FILE

    def read_block(self, n):
        fi, rest = divmod(n, self.blocks_per_file)
        group, inner = divmod(rest, BLOCKS_PER_GROUP)
        off = BLOCK + group * (BLOCK * (BLOCKS_PER_GROUP + 1)) + BLOCK + inner * BLOCK
        if fi >= len(self.handles):
            return b''
        h = self.handles[fi]
        h.seek(off)
        return h.read(BLOCK)

    def read(self, off, size):
        out = bytearray()
        while size > 0:
            n, skip = divmod(off, BLOCK)
            b = self.read_block(n)
            if not b:
                break
            part = b[skip:skip + size]
            out += part
            off += len(part); size -= len(part)
        return bytes(out)


MAGIC = b'MICROSOFT*XBOX*MEDIA'
img = Image(sys.argv[1])
base = None
total_blocks = len(img.files) * img.blocks_per_file
for n in range(total_blocks):
    b = img.read_block(n)
    if not b:
        break
    for skip in (0, SECTOR):
        if b[skip:skip + 20] == MAGIC:
            base = n * BLOCK + skip - 32 * SECTOR
            break
    if base is not None:
        break
print('base', hex(base) if base is not None else None, 'files', len(img.files))
vd = img.read(base + 32 * SECTOR, SECTOR)
root, root_size = struct.unpack('<II', vd[20:28])
print('root sector', root, 'size', root_size)
out = sys.argv[2]
count = total = 0


def walk(sector, size, path):
    global count, total
    tbl = img.read(base + sector * SECTOR, size)

    def node(off):
        global count, total
        if off * 4 + 14 > len(tbl):
            return
        e = tbl[off * 4:]
        left, right, start, length, attr, nlen = struct.unpack('<HHIIBB', e[:14])
        if left == 0xFFFF:
            return
        name = e[14:14 + nlen].decode('latin1')
        if left:
            node(left)
        p = os.path.join(path, name)
        if attr & 0x10:
            os.makedirs(p, exist_ok=True)
            if length:
                walk(start, length, p)
        else:
            with open(p, 'wb') as f:
                pos = base + start * SECTOR; left_b = length
                while left_b:
                    c = img.read(pos, min(left_b, 1 << 20))
                    f.write(c); pos += len(c); left_b -= len(c)
            count += 1; total += length
        if right:
            node(right)

    node(0)


os.makedirs(out, exist_ok=True)
walk(root, root_size, out)
print('extracted', count, 'files', total // 1048576, 'MB')
