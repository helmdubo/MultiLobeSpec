"""Independent conservative 1D slab reference, not a simulation of UE images.

Isotropic incident radiance 1 at both boundaries; homogeneous extinction;
isotropic elastic scattering. Analytic cell-average formal solution per discrete
ordinate. Radiance/source are cell averages, unlike B1's point samples. This
isolates lost orders and artificial damping even with full path coverage and
exact segment integration. Units: slab width=1, extinction=optical thickness.
"""
import pathlib, json
import numpy as np
ROOT=pathlib.Path(__file__).parent

def operator(tau, albedo, n=128, directions=64):
    m,w=np.polynomial.legendre.leggauss(directions//2)
    m,w=(m+1)/2,w/2  # quadrature on [0,1]; exact integral of constant and mu
    dx=1/n
    a=tau*dx/m
    t=np.exp(-a)
    f=-np.expm1(-a)/a
    i=np.arange(n)
    boundary=np.sum(.5*w[:,None]*f[:,None]*(t[:,None]**i+t[:,None]**(n-1-i)),axis=0)
    distance=np.abs(i[:,None]-i[None,:])
    kernel=np.zeros((n,n),dtype='float64')
    for weight,trans,average in zip(w,t,f):
        term=.5*weight*albedo*(1-trans)*average*trans**np.maximum(distance-1,0)
        np.fill_diagonal(term,weight*albedo*(1-average))
        kernel+=term
    assert np.max(np.abs(kernel.sum(axis=1)/albedo+boundary-1))<3e-13
    return boundary,kernel,m,w,t

def flux(j,tau,albedo,m,w,t):
    # Two outgoing hemispheres / total incoming flux (2*pi).
    n=len(j); i=np.arange(n)
    left=t**n+albedo*(1-t)*np.sum(j[None,:]*t[:,None]**i,axis=1)
    right=t**n+albedo*(1-t)*np.sum(j[None,:]*t[:,None]**(n-1-i),axis=1)
    escaped=float(np.sum(w*m*(left+right)))
    absorbed=float(2*tau*(1-albedo)*np.mean(j))
    return {'escaped_input_ratio':escaped,'absorbed_input_ratio':absorbed,
            'balance_relative_error':escaped+absorbed-1}

def run(tau,albedo,n=128):
    b,k,m,w,t=operator(tau,albedo,n)
    reference=np.linalg.solve(np.eye(n)-k,b)
    ref_flux=flux(reference,tau,albedo,m,w,t)
    assert abs(ref_flux['balance_relative_error'])<2e-12
    if albedo==1:assert np.max(np.abs(reference-1))<2e-12
    variants=[]
    for damping,shadow in [(0.35,1),(0.5,1),(1,1),(0.35,0.5)]:
        primary=(1-shadow)+shadow*b
        order=primary.copy();j=order.copy()
        for _ in range(3):
            order=damping*(k@order)
            j+=order
        variants.append({'damping':damping,'indirect_shadow_strength':shadow,'extra_orders':3,
                         'mean_J':float(j.mean()),'center_J':float(.5*(j[n//2-1]+j[n//2])),
                         'rms_reference_error':float(np.linalg.norm(j-reference)/np.linalg.norm(reference)),
                         **flux(j,tau,albedo,m,w,t)})
    # Constant radiance must be a fixed point in a white furnace. Zero extra
    # damping and no arbitrary range truncation are essential for that identity.
    return {'optical_thickness':tau,'albedo':albedo,'cells':n,'directions':64,
            'reference_mean_J':float(reference.mean()),'reference_center_J':float(.5*(reference[n//2-1]+reference[n//2])),
            'reference_flux':ref_flux,'variants':variants}


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', default='energy-reference.json')
    args=parser.parse_args()
    results=[run(tau,1) for tau in (.1,1,4,8)]
    results += [run(4,.9,n) for n in (32,64,128)]
    out={'scope':'independent 1D homogeneous cell-average reference; not GPU or user-scene measurement',
         'limits':'B1-like variants retain only order count/damping/shadow blend. No range cutoff, geometry, density aliasing, Lumen or native compositing; optimistic full coverage.',
         'reference_acceptance':'white furnace J=1 and escaped+absorbed=input within 2e-12',
         'results':results}
    pathlib.Path(args.output).write_text(json.dumps(out,indent=2))
    print('tau  albedo   center reference    center B1-like (.35/3orders)  escaped  missing balance')
    for a in results:
        b=a['variants'][0]
        print(f"{a['optical_thickness']:4g} {a['albedo']:6g} {a['reference_center_J']:18.8f} {b['center_J']:29.8f} {b['escaped_input_ratio']:8.5f} {-b['balance_relative_error']:8.5f}")
