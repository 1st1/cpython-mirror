import time
import inspect

def f1(a, b=1, *, z, **ar): pass


s1 = inspect.signature(f1)

for run in range(1, 6):
    started = time.monotonic()
    for i in range(10**4 * 2):
        s1.bind(1, 2, z=3, y=4)
    print("{}. {:.3f}s".format(run, time.monotonic() - started))



"""

====================================  ===========  ==============  ===============
function / call                       bind (3.4)   bind cache hit  bind cache miss
====================================  ===========  ==============  ===============
() / ()                               0.716s       0.746s  (-4%)   0.799s  (-10%)
(a, b=1) / (10)                       1.140s       0.910s  (+20%)  1.294s  (-12%)
(a, b=1, *ar) / (10, 20, 30, 40)      1.352s       1.145s  (+15%)  1.520s  (-11%)
(a, b=1, **ar) / (10, 20, z=30, y=4)  1.364s       1.233s  (+10%)  1.660s  (-18%)
(a, b=1, *, z, **ar) / (1,2,z=3,y=4)  1.499s       1.363s  (+10%)  1.897s  (-26%)

"""
