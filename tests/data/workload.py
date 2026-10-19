"""tests/data/workload.py -- allocation-heavy, deterministic Python workload
for preload_smoke.sh: dicts, lists, sets, strings, json round trips, sorting,
classes with __dict__, and a cyclic-garbage collection pass.  Prints digests
so output can be diffed between allocators."""
import gc
import hashlib
import json
import random
import sys

rng = random.Random(12345)


def digest(obj):
    return hashlib.sha256(repr(obj).encode()).hexdigest()[:16]


# dict churn: build, update, delete
d = {}
for i in range(200000):
    k = "k%d_%d" % (i % 5000, rng.randrange(1000))
    d[k] = d.get(k, 0) + i
for k in list(d)[::3]:
    del d[k]
print("dict", len(d), digest(sorted(d.items())[:1000]))

# lists of lists, sorting with keys
rows = [[rng.randrange(10**6), "x" * rng.randrange(1, 40), rng.random()] for _ in range(100000)]
rows.sort(key=lambda r: (r[0] % 997, r[1], r[2]))
print("sort", len(rows), digest(rows[:500]), digest(rows[-500:]))

# json round trip of a nested structure
doc = {"items": [{"id": i, "tags": ["t%d" % (i % 17)] * (i % 5), "v": i * 1.5,
                  "sub": {"a": list(range(i % 30)), "s": "y" * (i % 100)}}
                 for i in range(30000)]}
text = json.dumps(doc, sort_keys=True)
back = json.loads(text)
assert back == doc
print("json", len(text), hashlib.sha256(text.encode()).hexdigest()[:16])

# strings: join/split/replace
words = ["".join(chr(97 + rng.randrange(26)) for _ in range(rng.randrange(1, 12))) for _ in range(150000)]
blob = " ".join(words)
parts = blob.replace("a", "AA").split("e")
print("str", len(blob), len(parts), digest(parts[:200]))

# sets and frozensets
s1 = set(words[::2])
s2 = set(words[1::2])
print("set", len(s1 & s2), len(s1 | s2), len(s1 ^ s2))


# objects with cycles, then gc
class Node:
    def __init__(self, i):
        self.i = i
        self.peers = []
        self.payload = bytearray(i % 300)


nodes = [Node(i) for i in range(50000)]
for n in nodes:
    n.peers.append(nodes[(n.i * 7919) % len(nodes)])
total = sum(len(n.payload) for n in nodes)
del nodes
collected = gc.collect()
print("gc", total, collected >= 0)

# big bytes / bytearray growth
ba = bytearray()
for i in range(20000):
    ba += bytes([i % 256]) * (i % 97)
print("bytes", len(ba), hashlib.sha256(ba).hexdigest()[:16])
sys.stdout.flush()
