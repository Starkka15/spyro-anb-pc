"""Import dubbed FMV audio from a European PS2 disc for the Xbox XMV movies.

The Xbox release ships these cutscenes with English audio only (csz_10 has six
tracks, but the game always plays track 0). The European PS2 disc (SLES-54359)
has six tracks per movie (EN FR ES DE IT NL, same order as the Xbox csz_10),
cut from the same masters: the tracks line up with the Xbox audio at a fixed
offset and run the same length.

For each language this writes <out>/<LANG>/<movie>_60.xmv.ovl: a read overlay
holding that language's audio for every track-0 span of the XMV. The runtime
(RECOMP_READ_OVERLAY) serves those bytes in place of the English audio when the
game reads the movie, so the game files stay untouched and the language follows
the setting. Video bytes are never in an overlay.

Only the *_60 movies get overlays. The port always plays those (US mode and the
PAL60 path); the Xbox *_50 movies carry a different audio mix that the PS2
tracks don't line up with.

.ovl format (little-endian): "ROVL", u32 version 1, u32 span count, then
count x {u64 file offset, u32 length, u32 0}, then the span bytes in order.

Prototype: needs numpy and ffmpeg (to decode PS2 ADPCM and resample 48k->32k).

usage: ps2_dubs.py <EU PS2 .iso or MEDIA_P2.RKV> <xbox video dir> <out dir> [LANG ...]
"""
import os, struct, subprocess, sys
import numpy as np

FFMPEG = os.environ.get("FFMPEG", "G:/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.exe")
LANGS = {"EN": 0, "FR": 1, "ES": 2, "DE": 3, "IT": 4, "NL": 5}
# Xbox movie -> PS2 movie with six tracks. The EU disc has no csz_80 (with
# title); csz_80nt carries the same audio (identical on the US disc).
MOVIES = {
    "attract": "attract_50.pss", "csc_19": "csc_19_50.pss", "csc_20": "csc_20_50.pss",
    "csd_14": "csd_14_50.pss", "csd_15": "csd_15_50.pss", "cse_18": "cse_18_50.pss",
    "cse_19": "cse_19_50.pss", "csz_10": "csz_10_50.pss", "csz_80": "csz_80nt_50.pss",
    "csz_84": "csz_84_50.pss",
}
RATE = 32000


class Source:
    """MEDIA_P2.RKV, either a plain file or inside an ISO9660 image."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.base = 0
        self.f.seek(0)
        if self.f.read(4) != b"RKV2":
            self.base = self._iso_find(b"MEDIA_P2.RKV")
        self.entries = self._rkv_entries()

    def _iso_find(self, want):
        self.f.seek(16 * 2048)
        pvd = self.f.read(2048)
        if pvd[1:6] != b"CD001":
            raise SystemExit("not an RKV or an ISO9660 image")
        rec = pvd[156:190]
        lba, size = struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        self.f.seek(lba * 2048)
        d = self.f.read(size)
        i = 0
        while i < len(d):
            n = d[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048
                continue
            name = d[i + 33:i + 33 + d[i + 32]].split(b";")[0]
            if name.upper() == want:
                return struct.unpack_from("<I", d, i + 2)[0] * 2048
            i += n
        raise SystemExit("MEDIA_P2.RKV not found in the image")

    def _rkv_entries(self):
        self.f.seek(self.base)
        h = self.f.read(0x80)
        if h[:4] != b"RKV2":
            raise SystemExit("bad RKV header")
        n = struct.unpack_from("<I", h, 4)[0]
        ent = self.f.read(20 * n)
        names = self.f.read(65536)
        out = {}
        for i in range(n):
            no, _, sz, off, _ = struct.unpack_from("<5I", ent, i * 20)
            out[names[no:names.index(b"\0", no)].decode("latin1")] = (off, sz)
        return out

    def read(self, name):
        off, sz = self.entries[name]
        self.f.seek(self.base + off)
        return self.f.read(sz)


def pss_tracks(d):
    """Split private stream 1 (ff a1 00 NN) into per-track SShd/SSbd files."""
    tr = {}
    i, n = 0, len(d)
    while i + 4 <= n:
        if d[i:i + 3] != b"\0\0\1":
            i += 1
            continue
        sid = d[i + 3]
        if sid == 0xBA:
            i += 14 + (d[i + 13] & 7)
            continue
        if sid == 0xB9:
            break
        ln = struct.unpack_from(">H", d, i + 4)[0]
        p = i + 6
        if sid == 0xBD:
            pl = p + 3 + d[p + 2]
            tr.setdefault(d[pl + 3], bytearray()).extend(d[pl + 4:p + ln])
        i = p + ln
    for b in tr.values():
        j = b.find(b"SSbd")
        struct.pack_into("<I", b, j + 4, len(b) - j - 8)
    return tr


def decode_ads(ads):
    """PS2 ADPCM (SShd/SSbd) -> int16 stereo at 32 kHz, shape (n, 2)."""
    r = subprocess.run([FFMPEG, "-v", "error", "-f", "ads", "-i", "pipe:0", "-ar", str(RATE),
                        "-ac", "2", "-f", "s16le", "pipe:1"], input=bytes(ads), capture_output=True)
    if r.returncode:
        raise SystemExit("ffmpeg failed: " + r.stderr.decode(errors="replace"))
    return np.frombuffer(r.stdout, np.int16).reshape(-1, 2)


def xmv_audio_spans(d):
    """Byte spans of each packet's track-0 audio, in order."""
    if d[12:16] != b"xobX":
        raise ValueError("not an XMV file")
    first = struct.unpack_from("<I", d, 4)[0]
    nt = struct.unpack_from("<H", d, 32)[0]
    for k in range(nt):
        comp, ch, rate, bits, _ = struct.unpack_from("<HHIHH", d, 36 + 12 * k)
        if (comp, ch, rate, bits) != (1, 2, RATE, 16):
            raise ValueError("unexpected audio format %r" % ((comp, ch, rate, bits),))
    pos = 36 + 12 * nt
    size = first - pos
    spans = []
    while pos < len(d) and size:
        nsize = struct.unpack_from("<I", d, pos)[0]
        vsz = (struct.unpack_from("<I", d, pos + 4)[0] & 0x7FFFFF) - 4 * nt
        asz = struct.unpack_from("<I", d, pos + 12)[0] & 0x7FFFFF
        spans.append((pos + 12 + 4 * nt + vsz, asz))
        pos += size
        size = nsize
    return spans


def find_lag(ref, x):
    """Samples to shift x by so it lines up with ref (mono float arrays)."""
    m = min(len(ref), len(x))
    s = m // 2
    a = ref[s:s + RATE * 8]
    b = x[s:s + RATE * 8]
    a = a - a.mean()
    b = b - b.mean()
    n = 1 << int(np.ceil(np.log2(2 * len(a))))
    c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)
    k = int(np.argmax(np.abs(c)))
    score = abs(c[k]) / np.sqrt((a * a).sum() * (b * b).sum())
    return (k if k < n // 2 else k - n), score


def build_overlay(xmv_path, en, dubs):
    """Return {lang: ovl bytes} for one XMV, plus the measured lag and match."""
    d = open(xmv_path, "rb").read()
    spans = xmv_audio_spans(d)
    orig = np.frombuffer(b"".join(d[o:o + s] for o, s in spans), np.int16).reshape(-1, 2)
    ref = orig.astype(np.float64).mean(1)
    enm = en.astype(np.float64).mean(1)
    lag, score = find_lag(ref, enm)
    if score < 0.8:
        raise SystemExit("%s: PS2 English doesn't match the Xbox audio (%.2f)" % (xmv_path, score))
    gain = np.sqrt((ref ** 2).mean() / max((enm ** 2).mean(), 1.0))
    head = b"ROVL" + struct.pack("<II", 1, len(spans)) + b"".join(
        struct.pack("<QII", o, s, 0) for o, s in spans)
    out = {}
    for lang, dub in dubs.items():
        pcm = np.zeros_like(orig)
        src = dub[max(0, -lag):]
        dst = max(0, lag)
        n = min(len(pcm) - dst, len(src))
        pcm[dst:dst + n] = np.clip(src[:n].astype(np.float64) * gain, -32768, 32767).astype(np.int16)
        out[lang] = head + pcm.tobytes()
    return out, lag, score


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    src, vdir, odir = sys.argv[1:4]
    langs = [l.upper() for l in sys.argv[4:]] or [l for l in LANGS if l != "EN"]
    for l in langs:
        if l not in LANGS or l == "EN":
            raise SystemExit("unknown language %s (FR ES DE IT NL)" % l)
    rkv = Source(src)
    for l in langs:
        os.makedirs(os.path.join(odir, l), exist_ok=True)
    for movie, pss in MOVIES.items():
        tr = pss_tracks(rkv.read(pss))
        if len(tr) != 6:
            raise SystemExit("%s has %d audio tracks; this isn't the European disc" % (pss, len(tr)))
        name = movie + "_60.xmv"
        ovl, lag, score = build_overlay(os.path.join(vdir, name), decode_ads(tr[0]),
                                        {l: decode_ads(tr[LANGS[l]]) for l in langs})
        for l, b in ovl.items():
            open(os.path.join(odir, l, name + ".ovl"), "wb").write(b)
        print("%-14s lag %+5d  match %.2f  %.1f MB per language" % (name, lag, score, len(b) / 1e6))


if __name__ == "__main__":
    main()
