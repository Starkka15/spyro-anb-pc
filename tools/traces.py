# Reapplies the DBG traces to the generated code after a re-lift (the lifter wipes them).
import glob
n = chr(92) + 'n'
PRE = ' { extern int fprintf(void*,const char*,...); extern void* __cdecl __acrt_iob_func(unsigned); static unsigned c; '
TRACES = {
    'loc_0031AAC7: ;': 'if (c++ < 6 || !(c & 0xFF)) fprintf(__acrt_iob_func(2), "[dowork] ptr=%08X val=%08X' + n + '", MEM32(0x3341D0), MEM32(MEM32(0x3341D0)));',
    'loc_003350F2: ;': 'if (!(c++ & 31)) fprintf(__acrt_iob_func(2), "[xmvt] vbl=%u start=%u F8=%u E4=%u BC=%X C8=%u E8=%u 3C=%u B0=%X' + n + '", MEM32(ebp - 32), MEM32(esi + 0xB8), MEM32(esi + 0xF8), MEM32(esi + 0xE4), MEM32(esi + 0xBC), MEM32(esi + 0xC8), MEM32(esi + 0xE8), MEM32(esi + 0x3C), MEM32(esi + 0xB0));',
    'loc_00334BF4: ;': 'if (c++ < 400) fprintf(__acrt_iob_func(2), "[xmvc] now=%u ts=%u tick=%u:%u div=%u:%u' + n + '", eax, MEM32(ebp + 0xC), MEM32(ebp - 8), MEM32(ebp - 4), MEM32(0x35C1D8), MEM32(0x35C1DC));',
}
for f in glob.glob('H:/spyro-xbox/src/recomp/gen/recomp_00*.c'):
    s = open(f, newline='', encoding='latin1').read()
    t = s
    for k, v in TRACES.items():
        if t.count(k) == 1:
            t = t.replace(k, k + PRE + v + ' } /* DBG */')
            print('trace', k, f[-14:])
    if t != s:
        open(f, 'w', newline='', encoding='latin1').write(t)
