import time
import inspect

def f1(a, b, c=None, *args, d=1, **kw): pass


s1 = inspect.signature(f1)

started = time.monotonic()
for i in range(10**4):
    s1.bind(1, 2, c=3, d=100)
print("{:.3f}".format(time.monotonic() - started))
