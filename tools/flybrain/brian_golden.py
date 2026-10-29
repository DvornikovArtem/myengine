"""Golden spikes of Brian2 for tests/flybrain/BrianGolden.h (the test TestBrianGolden in FlyBrainTests.cpp).

The equations, constants and step of Shiu et al. 2024 (github.com/philshiu/Drosophila_brain_model, model.py, MIT) with an
extra constant drive I. Needs brian2 (pip install brian2, numpy target, no compiler):

    python brian_golden.py > golden.txt
"""
import numpy as np
from brian2 import *
prefs.codegen.target = 'numpy'
defaultclock.dt = 0.1*ms
v_0=-52*mV; v_rst=-52*mV; v_th=-45*mV; t_mbr=20*ms; tau=5*ms; t_rfc=2.2*ms; t_dly=1.8*ms; w_syn=0.275*mV
N=60
pre=[];post=[];w=[]
for i in range(N):
    for k in range(1, 11):
        j=(i*7+k*13)%N
        if j==i: continue
        pre.append(i); post.append(j); w.append((-1 if i%3==0 else 1)*(1+(i*k)%15))
I=[(8+(i%5)*3.0) if i%4==0 else 0.0 for i in range(N)]
# merge duplicate pairs like the builder does
d={}
for p,q,ww in zip(pre,post,w): d[(p,q)]=d.get((p,q),0)+ww
pairs=[(p,q,ww) for (p,q),ww in sorted(d.items()) if ww!=0]
eqs='''
dv/dt = (v_0 - v + g + I) * (1./t_mbr) : volt (unless refractory)
dg/dt = -g / tau : volt (unless refractory)
rfc : second
I : volt
'''
neu=NeuronGroup(N, eqs, threshold='v > v_th', reset='v = v_rst; g = 0 * mV', refractory='rfc', method='linear')
neu.v=v_0; neu.g=0*mV; neu.rfc=t_rfc; neu.I=np.array(I)*mV
syn=Synapses(neu, neu, 'w : volt', on_pre='g += w', delay=t_dly)
syn.connect(i=[p for p,_,_ in pairs], j=[q for _,q,_ in pairs]); syn.w=np.array([x for _,_,x in pairs])*w_syn
mon=SpikeMonitor(neu)
run(500*ms)
spikes=sorted(zip(np.round(np.asarray(mon.t/ms)/0.1).astype(int).tolist(), np.asarray(mon.i).tolist()))
print(len(spikes), "spikes")
flat=[]
for t,i in spikes: flat += [t,i]
lines=[]
for k in range(0,len(flat),16):
    lines.append("        "+", ".join(str(x) for x in flat[k:k+16])+",")
print("\n".join(lines))
