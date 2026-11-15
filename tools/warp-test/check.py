import numpy as np, sys
W,H=1920,1080
def img(n): return np.fromfile(n,np.uint8).reshape(H,W,4)[...,:3].astype(np.float32)/255
def psnr(a,b,m=None):
    d=(a-b)**2
    if m is not None: d=d[m]
    return 10*np.log10(1/max(d.mean(),1e-12))
f0,f1,f15,f2=img('f0.rgba'),img('f1.rgba'),img('f15.rgba'),img('f2.rgba')
o=[img('out%d.rgba'%i) for i in range(4)]
fw,fh=[int(x) for x in sys.argv[1:3]]
fl=np.fromfile('flow.f16',np.float16).reshape(fh,fw,4).astype(np.float32)
fx,fy,conf=fl[...,0]*W,fl[...,1]*H,fl[...,2]
print('passthrough exact: out0==f0 %s, out1==f1 %s'%(np.array_equal(o[0],f0),np.array_equal(o[1],f1)))
# regions (full-res masks)
yy,xx=np.mgrid[0:H,0:W]
hud=((yy>=40)&(yy<110)&(xx>=60)&(xx<520))|((yy>=H-90)&(yy<H-40)&(xx>=W-420)&(xx<W-60))
objm=(yy>=390)&(yy<600)&(xx>=860)&(xx<1090)
border=(yy<40)|(yy>=H-40)|(xx<60)|(xx>=W-60)
bgm=~hud&~objm&~border
near=np.zeros_like(hud)
# flow stats on the flow grid
sy,sx=H/fh,W/fw
gy,gx=((np.mgrid[0:fh,0:fw]+0.5)*np.array([sy,sx])[:,None,None]).astype(int)
gb=bgm[gy,gx]; gh=hud[gy,gx]
gyc,gxc=gy,gx
inner_hud=((gyc>=52)&(gyc<98)&(gxc>=80)&(gxc<500))|((gyc>=H-80)&(gyc<H-50)&(gxc>=W-400)&(gxc<W-80))
objin=(gyc>=425)&(gyc<560)&(gxc>=905)&(gxc<1040)
# only where there is something to see moving: cells with detail (a flat area has no measurable motion and needs none)
from scipy.ndimage import uniform_filter
lum=f1.mean(-1); var=uniform_filter(lum*lum,24)-uniform_filter(lum,24)**2
det=np.sqrt(np.maximum(var,0))[gy,gx]>0.06
gb=gb&det
print('background motion (cells with detail): median (%.2f, %.2f) px, truth (22, -7); within 1 px: %.1f%%; mean confidence %.2f'%(np.median(fx[gb]),np.median(fy[gb]),100*np.mean((abs(fx[gb]-22)<1)&(abs(fy[gb]+7)<1)),conf[gb].mean()))
print('HUD motion (inside): median (%.2f, %.2f) px, truth (0, 0); |v|<0.5: %.1f%%'%(np.median(fx[inner_hud]),np.median(fy[inner_hud]),100*np.mean(np.hypot(fx[inner_hud],fy[inner_hud])<0.5)))
print('object motion (inside): median (%.2f, %.2f) px, truth (-9, 12); within 1 px: %.1f%%'%(np.median(fx[objin]),np.median(fy[objin]),100*np.mean((abs(fx[objin]+9)<1)&(abs(fy[objin]-12)<1))))
for name,m in (('whole frame',None),('background',bgm),('HUD',hud),('object area',objm)):
    print('%-12s PSNR vs truth  half frame ahead: repeat %.1f dB -> warped %.1f dB   |  one frame ahead: repeat %.1f dB -> warped %.1f dB'%(
        name,psnr(f1,f15,m),psnr(o[2],f15,m),psnr(f1,f2,m),psnr(o[3],f2,m)))
cross=np.zeros_like(hud); cross[300,1385:1416]=True; cross[285:316,1400]=True
print('crosshair (one-pixel lines over the moving scene): pixels changed by the warp: %d of %d'%((np.abs(o[2]-f1)[cross].max(-1)>0).sum(),cross.sum()))
print('HUD pixels changed by the warp (half): %.2f%%, max change %.3f'%(100*np.mean(np.abs(o[2]-f1)[hud].max(-1)>0.02),np.abs(o[2]-f1)[hud].max()))
d=np.abs(o[2]-f15).max(-1); d0=np.abs(f1-f15).max(-1)
print('pixels off by more than 16/255 (half frame ahead): repeat %.1f%%  warped %.1f%%'%(100*(d0>16/255).mean(),100*(d>16/255).mean()))
d=np.abs(o[3]-f2).max(-1); d0=np.abs(f1-f2).max(-1)
print('pixels off by more than 16/255 (one frame ahead):  repeat %.1f%%  warped %.1f%%'%(100*(d0>16/255).mean(),100*(d>16/255).mean()))
import os
if os.path.exists('flow1.f16') and len(sys.argv)>3 and sys.argv[3]=='shift':
    g=np.fromfile('flow1.f16',np.float16).astype(np.float32)
    print('one motion for the frame: (%.2f, %.2f) px, the pan is (22, -7)'%(g[0]*W,g[1]*H))
    # a shifted frame holds only pixels the game drew: compare with f1 moved by the rounded shift
    dx,dy=int(round(g[0]*W*0.5)),int(round(g[1]*H*0.5))
    sh=np.roll(f1,(dy,dx),(0,1)); m=bgm&(np.roll(bgm,(dy,dx),(0,1)))
    m[:40]=m[-40:]=False; m[:,:40]=m[:,-40:]=False
    print('half a frame ahead = the frame moved by (%d, %d) whole pixels: %.2f%% of background pixels differ from that'%(dx,dy,100*(np.abs(o[2]-sh).max(-1)[m]>1/255).mean()))
