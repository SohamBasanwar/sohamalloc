#!/usr/bin/env python3
"""Allocation-heavy CPython workload for the `python` benchmarks.

Exercises the patterns that reach the C allocator in real Python programs:
dict/list growth (realloc), medium strings and bytes (> 512 B bypass pymalloc),
JSON round trips, object graphs that die young, sorting, and a long-lived
cache with churn.  Run with PYTHONMALLOC=malloc to route *every* object
through malloc (the `python-malloc` benchmark).

Usage: py_alloc.py [SCALE]   (default 1; work grows linearly)
Prints a checksum so a corrupted run is detectable.
"""
import json
import random
import sys


class Node:
    __slots__ = ("key", "val", "kids")

    def __init__(self, key, val):
        self.key = key
        self.val = val
        self.kids = []


def tree(rng, depth, fan):
    n = Node(rng.random(), "v" * rng.randrange(1, 64))
    if depth > 0:
        n.kids = [tree(rng, depth - 1, fan) for _ in range(fan)]
    return n


def walk(n):
    s = len(n.val)
    for k in n.kids:
        s += walk(k)
    return s


def phase_dicts(rng, n):
    acc = 0
    for r in range(n):
        d = {}
        for i in range(2000):
            d["k%d_%d" % (i, r)] = [i] * rng.randrange(1, 40)
        acc += sum(len(v) for v in d.values())
        for k in list(d)[::3]:
            del d[k]
        acc += len(d)
    return acc


def phase_strings(rng, n):
    acc = 0
    for r in range(n):
        parts = []
        for i in range(3000):
            parts.append(("%08x" % rng.getrandbits(32)) * rng.randrange(1, 200))
        s = "|".join(parts)
        acc += len(s.split("|"))
        b = bytearray()
        for p in parts[:600]:
            b += p.encode()
        acc += len(bytes(b).upper())
    return acc


def phase_json(rng, n):
    acc = 0
    for r in range(n):
        doc = [{"id": i, "name": "user%d" % i, "tags": ["t%d" % j for j in range(i % 17)],
                "score": rng.random(), "blob": "x" * rng.randrange(0, 2000)}
               for i in range(1500)]
        text = json.dumps(doc)
        back = json.loads(text)
        acc += len(text) + len(back)
    return acc


def phase_objects(rng, n):
    acc = 0
    for r in range(n):
        t = tree(rng, 7, 4)
        acc += walk(t)
        lst = [(rng.random(), "s%d" % i) for i in range(40000)]
        lst.sort()
        acc += len(lst)
    return acc


def phase_cache(rng, n):
    cache = {}
    acc = 0
    for i in range(n * 60000):
        k = rng.randrange(20000)
        if k in cache:
            acc += len(cache[k])
            if rng.random() < 0.3:
                del cache[k]
        else:
            cache[k] = bytes(rng.randrange(8, 4096))
    return acc + len(cache)


def main():
    scale = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    rng = random.Random(42)
    total = 0
    total += phase_dicts(rng, 6 * scale)
    total += phase_strings(rng, 4 * scale)
    total += phase_json(rng, 3 * scale)
    total += phase_objects(rng, 2 * scale)
    total += phase_cache(rng, 2 * scale)
    print("checksum", total)


if __name__ == "__main__":
    main()
