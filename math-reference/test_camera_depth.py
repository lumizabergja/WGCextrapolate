import numpy as np
from camera_depth import fit_rotation,estimate_depth,decode_nvofa
rng=np.random.default_rng(12)
K=np.array([[960.,0,959.5],[0,960.,539.5],[0,0,1.]])
p=np.column_stack((rng.uniform(-0.8,0.8,500),rng.uniform(-0.5,0.5,500),np.ones(500)))
z=rng.uniform(2,15,len(p));X=p*z[:,None]
yaw=0.04;R=np.array([[np.cos(yaw),0,np.sin(yaw)],[0,1,0],[-np.sin(yaw),0,np.cos(yaw)]])
t=np.array([0.16,-0.04,0.09]);B=np.linalg.norm(t)
def project(X):
    p=X@K.T;return p[:,:2]/p[:,2:]
xy=project(X);rot=project(X@R.T)
Rhat,mask=fit_rotation(xy,rot,K);assert np.max(np.abs(Rhat-R))<1e-10
assert estimate_depth(xy,rot,K,R)[0] is None
prev=project(X@R.T+t);prev[:50]=prev[:50]+rng.uniform(-150,150,(50,2))
th,zh,valid=estimate_depth(xy,prev,K,R,baseline=B)
assert valid[50:].sum()>440 and valid[:50].sum()<5
assert np.linalg.norm(th-t)<1e-8
assert np.nanmedian(np.abs(zh[50:]-z[50:])/z[50:])<1e-8
noisy=project(X@R.T+t);noisy=xy+np.round((noisy-xy)*32)/32+rng.normal(0,0.03,noisy.shape)
th2,zh2,v2=estimate_depth(xy,noisy,K,R,baseline=B,epipolar_tol=0.015)
assert v2.sum()>400
err=np.nanmedian(np.abs(zh2-z)/z)
assert err<0.02
raw=np.array([32,-64,0,16,999,999],dtype='<i2').tobytes();coords,flow=decode_nvofa(raw,8,4,3)
assert np.allclose(flow,[[1,-2],[0,.5]]) and len(coords)==2
print('PASS: pure rotation fit; no invented depth under pure rotation')
print('PASS: translation and depth with 10% corrupted correspondences (synthetic)')
print(f'PASS: quantized/noisy synthetic flow, valid {v2.sum()}/{len(z)}, median relative depth error {err:.4%}')
print('PASS: pitched S10.5 backward-flow decoding')
