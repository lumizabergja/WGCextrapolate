"""CPU reference, not a live GPU or NGX backend. Requires NumPy.
Coordinates: x right, y down, z forward; column-vector transforms.
NvOFA direction: current -> previous. R,t transform current points to previous.
"""
import numpy as np

def rays(xy, K):
    xy=np.asarray(xy,dtype=float)
    return np.column_stack((xy,np.ones(len(xy)))) @ np.linalg.inv(K).T

def unit(v):
    return v / np.maximum(np.linalg.norm(v,axis=-1,keepdims=True),1e-15)

def decode_nvofa(raw, width, height, pitch_elements):
    """Signed int16 vector pairs, S10.5 pixels; pitch in vector pairs, grid 4.
    Grid sample centers use pixel-center convention; validate on device.
    """
    h,w=(height+3)//4,(width+3)//4
    if pitch_elements<w: raise ValueError('invalid pitch')
    a=np.frombuffer(raw,dtype='<i2').reshape(-1,2)
    if len(a)<h*pitch_elements: raise ValueError('short flow buffer')
    flow=a[:h*pitch_elements].reshape(h,pitch_elements,2)[:,:w].astype(float)/32
    yy,xx=np.mgrid[:h,:w]
    xy=np.stack((np.minimum(xx*4+1.5,width-1),np.minimum(yy*4+1.5,height-1)),axis=-1).reshape(-1,2)
    return xy,flow.reshape(-1,2)

def fit_rotation(xy, prev_xy, K, eligible=None, threshold=0.003):
    """Rotation-only robust Wahba fit. Use ONLY during known rotation-only motion.
    threshold is angular error in approximate radians (unit-ray chord).
    """
    a,b=unit(rays(xy,K)),unit(rays(prev_xy,K))
    mask=np.isfinite(a).all(1)&np.isfinite(b).all(1)
    if eligible is not None: mask &= np.asarray(eligible,bool)
    w=mask.astype(float)
    if mask.sum()<8: raise ValueError('insufficient valid rays')
    a=np.nan_to_num(a); b=np.nan_to_num(b)
    for _ in range(8):
        u,_,vh=np.linalg.svd((b*w[:,None]).T@a)
        d=np.eye(3);d[2,2]=np.linalg.det(u@vh)
        R=u@d@vh
        e=np.linalg.norm(a@R.T-b,axis=1)
        w=mask*np.minimum(1,threshold/np.maximum(e,1e-12))
    inliers=mask&(e<threshold)
    return R,inliers

def estimate_depth(xy, prev_xy, K, R, eligible=None, baseline=None,
                   min_parallax=0.0015, epipolar_tol=0.003, trials=128, seed=13):
    """Known calibrated rotation -> translation direction + depth, up to common scale.
    R must correspond to the exact captured pair, not late mouse input.
    baseline=None means unit translation; a known baseline makes depths metric.
    Returns (t, depth, valid_mask). NaN depth = unobservable or rejected.
    """
    p=rays(xy,K);q=unit(rays(prev_xy,K));rp=p@np.asarray(R).T
    n=np.cross(unit(rp),q);par=np.linalg.norm(n,axis=1)
    m=np.isfinite(n).all(1)&(par>min_parallax)
    if eligible is not None:m &= np.asarray(eligible,bool)
    ids=np.flatnonzero(m);depth=np.full(len(p),np.nan)
    if len(ids)<12:return None,depth,np.zeros(len(p),bool)
    nn=unit(n[ids]);rng=np.random.default_rng(seed)
    best=np.zeros(len(ids),bool)
    for _ in range(trials):
        i,j=rng.choice(len(ids),2,replace=False)
        v=np.cross(nn[i],nn[j]);l=np.linalg.norm(v)
        if l<0.05:continue
        v/=l;inl=np.abs(nn@v)<epipolar_tol
        if inl.sum()>best.sum():best=inl
    if best.sum()<max(12,0.5*len(ids)):return None,depth,np.zeros(len(p),bool)
    _,s,vh=np.linalg.svd(nn[best],full_matrices=False)
    if s[1]<0.1*s[0]:return None,depth,np.zeros(len(p),bool)
    t=vh[-1];ids=ids[best];a=np.cross(q[ids],rp[ids]);den=np.sum(a*a,axis=1)
    z=-np.sum(a*np.cross(q[ids],t),axis=1)/np.maximum(den,1e-15)
    if np.sum(z>0)<np.sum(z<0):t=-t;z=-z
    if baseline is not None and (not np.isfinite(baseline) or baseline<=0):raise ValueError('invalid baseline')
    scale=1.0 if baseline is None else baseline;t*=scale;z*=scale
    pp=rp[ids]*z[:,None]+t
    reproj=np.linalg.norm(unit(pp)-q[ids],axis=1)
    good=(z>0)&(pp[:,2]>0)&(reproj<0.003)
    if good.sum()<max(12,0.6*len(ids)):return None,depth,np.zeros(len(p),bool)
    depth[ids[good]]=z[good]
    return t,depth,np.isfinite(depth)
