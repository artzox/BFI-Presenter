# Synthetic test frames: a panning textured background, an independently moving object, a static HUD.
import numpy as np, sys
from scipy.ndimage import gaussian_filter, map_coordinates
W,H=1920,1080
rng=np.random.default_rng(3)
# large background canvas with structure at several scales + hard edges
CW,CH=W+600,H+400
bg=np.full((CH,CW,3),0.35,np.float32)
yy,xx=np.mgrid[0:CH,0:CW]
# "dead leaves": overlapping shapes of random size and colour, the usual stand-in for natural pictures
for i in range(900):
    cx,cy=rng.uniform(0,CW),rng.uniform(0,CH); r=min(400,8/ (rng.uniform(0.02,1)**1.0)); col=rng.uniform(0.02,1,3)**1.6
    x0,x1,y0,y1=int(max(0,cx-r)),int(min(CW,cx+r)),int(max(0,cy-r)),int(min(CH,cy+r))
    if x1<=x0 or y1<=y0: continue
    if i%3: m=((xx[y0:y1,x0:x1]-cx)**2+(yy[y0:y1,x0:x1]-cy)**2)<r*r
    else: m=np.ones((y1-y0,x1-x0),bool)
    shade=1+0.25*np.sin(xx[y0:y1,x0:x1]*rng.uniform(0.02,0.3)+yy[y0:y1,x0:x1]*rng.uniform(0.02,0.3))
    bg[y0:y1,x0:x1][m]=(col*shade[...,None])[m]
bg+=0.02*rng.standard_normal(bg.shape).astype(np.float32)   # texture grain (moves with the picture)
bg[CH-500:,:]=bg[CH-500:,:]*0.15+0.5                        # a low-contrast area (haze)
bg=np.clip(bg,0,1)
for c in range(3): bg[...,c]=gaussian_filter(bg[...,c],0.7)
orng=np.random.default_rng(11)
obj=np.zeros((160,160,3),np.float32)
for i in range(60):
    x,y,r=orng.integers(0,160),orng.integers(0,160),orng.integers(6,40)
    obj[max(0,y-r):y+r,max(0,x-r):x+r]=orng.uniform(0,1,3)
def frame(t, bgv=(22.0,-7.0), objv=(-9.0,12.0), hud=True):
    ox,oy=300-bgv[0]*t,200-bgv[1]*t        # background moves by bgv px per frame on screen
    ys,xs=np.mgrid[0:H,0:W].astype(np.float32)
    f=np.stack([map_coordinates(bg[...,c],[ys+oy,xs+ox],order=3,mode='reflect') for c in range(3)],-1)
    px,py=900+objv[0]*t,400+objv[1]*t
    x0,y0=int(round(px)),int(round(py))
    f[y0:y0+160,x0:x0+160]=obj
    if hud:
        f[40:110,60:520]=0.08
        f[55:95,80:500:6]=0.95             # text-like bars
        f[H-90:H-40,W-420:W-60]=np.array([0.1,0.5,0.9])
        f[300,1385:1416]=1.0; f[285:316,1400]=1.0   # a crosshair: one-pixel lines over the moving scene
    return np.clip(f,0,1)
import sys
kw=dict(bgv=(0.0,0.0)) if len(sys.argv)>1 and sys.argv[1]=='still' else {}
for name,t in (('f0',0.0),('f1',1.0),('f15',1.5),('f2',2.0)):
    a=(frame(t,**kw)*255+0.5).astype(np.uint8)
    np.dstack([a,np.full((H,W,1),255,np.uint8)]).tofile(name+'.rgba')
print('ok')
