import struct,sys,os
d=sys.argv[1]; out=sys.argv[2]
L=['// Generated from shaders/ by build_mingw.sh (tools/genspv.py) -- do not edit','#pragma once','#include <cstdint>']
for name,f in [('kVertSpv','fullscreen.vert.spv'),('kBfiSpv','bfi.frag.spv'),('kMeasSpv','measure.frag.spv'),
                ('kBfiComp8Spv','bfi8.comp.spv'),('kBfiComp10Spv','bfi10.comp.spv'),('kBfiComp16Spv','bfi16.comp.spv'),('kMeasCompSpv','measure.comp.spv'),('kAplCompSpv','apl.comp.spv'),
                ('kWarpLumaSpv','warp0.comp.spv'),('kWarpDownSpv','warp1.comp.spv'),('kWarpFlowSpv','warp2.comp.spv'),('kWarpSmoothSpv','warp3.comp.spv'),('kGameFlowSpv','gameflow.comp.spv'),
                ('kCrtWarpSpv','crtwarp.comp.spv'),('kCrtPattern0Spv','crtPattern0.spv'),('kCrtPattern1Spv','crtPattern1.spv'),
                ('kCrtSharpenSpv','crtSharpen.spv'),('kCrtSoftenSpv','crtSoften.spv'),('kCrtGrainRawSpv','crtGrainRaw.spv'),('kCrtTailSpv','crtTail.spv')]:
    b=open(os.path.join(d,f),'rb').read(); w=struct.unpack('<%dI'%(len(b)//4),b)
    L.append('static const uint32_t %s[] = {'%name)
    for i in range(0,len(w),8): L.append('    '+', '.join('0x%08x'%x for x in w[i:i+8])+',')
    L.append('};')
open(out,'w').write('\n'.join(L)+'\n')
