import struct,sys,os
d=sys.argv[1]; out=sys.argv[2]
L=['// Generated from shaders/ by build_mingw.sh (tools/genspv.py) -- do not edit','#pragma once','#include <cstdint>']
for name,f in [('kVertSpv','fullscreen.vert.spv'),('kBfiSpv','bfi.frag.spv'),('kMeasSpv','measure.frag.spv')]:
    b=open(os.path.join(d,f),'rb').read(); w=struct.unpack('<%dI'%(len(b)//4),b)
    L.append('static const uint32_t %s[] = {'%name)
    for i in range(0,len(w),8): L.append('    '+', '.join('0x%08x'%x for x in w[i:i+8])+',')
    L.append('};')
open(out,'w').write('\n'.join(L)+'\n')
