from pathlib import Path
import struct,re,sys,os
ELF_PATH=os.environ.get('RETROLOOPER_HOST_ELF','/tmp/retrolooper-mpc-3.10.0.49.elf')
B=Path(ELF_PATH).read_bytes()
ph=struct.unpack_from('<I',B,28)[0];step,n=struct.unpack_from('<HH',B,42)
SEG=[struct.unpack_from('<8I',B,ph+i*step) for i in range(n)]
def addr(off):
 for t,o,a,p,z,*rest in SEG:
  if t==1 and o<=off<o+z:return a+off-o
 raise ValueError(hex(off))
def offset(a):
 for t,o,v,p,z,*rest in SEG:
  if t==1 and v<=a<v+z:return o+a-v
 raise ValueError(hex(a))
def word(a):return struct.unpack_from('<I',B,offset(a))[0]
def refs(a):
 packed=struct.pack('<I',a);at=0
 while True:
  at=B.find(packed,at)
  if at<0:return
  if not at%4:
   try:yield addr(at)
   except ValueError:pass
  at+=1
def vtables(name):
 name_at=B.find(name.encode()+b'\0');assert name_at>=0;name_a=addr(name_at);print(name,'name',hex(name_a))
 for r in refs(name_a):
  ti=r-4
  for use in refs(ti):
   try:fs=[word(use+4+i*4) for i in range(10)]
   except ValueError:continue
   if all(0x4af000<=f<0x4d00000 for f in fs[:2]):print('typeinfo',hex(ti),'vtable',hex(use+4),'funcs',*[hex(v) for v in fs])
if __name__=='__main__':
 for name in sys.argv[1:]:vtables(name)
