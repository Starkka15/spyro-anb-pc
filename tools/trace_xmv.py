p='H:/spyro-xbox/src/recomp/gen/recomp_0048.c'; s=open(p,newline='').read()
a='loc_002B6D06: ;'
assert s.count(a)==1
n=chr(92)+'n'
s=s.replace(a,a+' { extern int fprintf(void*,const char*,...); extern void* __cdecl __acrt_iob_func(unsigned); static unsigned c, nf; if (MEM32(esp + 0x10) == 1) nf++; if ((++c % 32) == 0) fprintf(__acrt_iob_func(2), "[xmvf] calls=%u newframes=%u'+n+'", c, nf); } /* DBG */')
open(p,'w',newline='').write(s)
