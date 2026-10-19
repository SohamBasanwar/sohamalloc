#!/usr/bin/env python3
"""Generate a large, template-heavy C++17 translation unit for the `gxx` workload.

The compiler (cc1plus) is an allocation-heavy program: it builds trees, hash
tables and GC'd arenas, and frees a great many small objects.  This generator
emits N "modules", each instantiating a distinct mix of standard-library
containers, variadic templates, recursive constexpr templates and lambdas, so
the compile is dominated by template instantiation and overload resolution.

Usage: gen_cxx.py OUT.cpp [N_MODULES]
Deterministic: the same N always produces byte-identical output.
"""
import random
import sys

HEADER = r"""
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

template <int N> struct Fib { static constexpr long value = Fib<N - 1>::value + Fib<N - 2>::value; };
template <> struct Fib<1> { static constexpr long value = 1; };
template <> struct Fib<0> { static constexpr long value = 0; };

template <typename... Ts> struct TypeList {};
template <typename L> struct Length;
template <typename... Ts> struct Length<TypeList<Ts...>> { static constexpr int value = sizeof...(Ts); };

template <typename T, int Depth> struct Nest { using type = std::vector<typename Nest<T, Depth - 1>::type>; };
template <typename T> struct Nest<T, 0> { using type = T; };

template <typename... Ts>
auto sum_all(Ts... ts) { return (static_cast<long>(ts) + ... + 0L); }

template <typename F, typename... Args>
auto apply_twice(F f, Args... args) { return f(f(args...)); }

template <typename Variant>
long visit_len(const Variant &v) {
  return std::visit([](const auto &x) -> long {
    using X = std::decay_t<decltype(x)>;
    if constexpr (std::is_arithmetic_v<X>) return static_cast<long>(x);
    else return static_cast<long>(x.size());
  }, v);
}
"""

SCALARS = ["int", "long", "double", "unsigned", "short", "char", "float", "long long"]
STRINGY = ["std::string", "std::vector<int>", "std::vector<std::string>"]


def module(i, rng):
    k = SCALARS[rng.randrange(len(SCALARS))]
    v = STRINGY[rng.randrange(len(STRINGY))]
    depth = rng.randrange(1, 4)
    fib = rng.randrange(10, 40)
    nvar = rng.randrange(3, 7)
    var_types = ", ".join(rng.sample(SCALARS + STRINGY, nvar))
    tup = ", ".join(rng.sample(SCALARS + STRINGY, rng.randrange(3, 8)))
    return f"""
namespace m{i} {{
struct Rec{i} {{
  {k} key; {v} val; std::optional<std::string> tag;
  bool operator<(const Rec{i} &o) const {{ return key < o.key; }}
  bool operator==(const Rec{i} &o) const {{ return key == o.key; }}
}};
struct Hash{i} {{ size_t operator()(const Rec{i} &r) const {{ return std::hash<long long>()((long long)r.key) ^ {i}u; }} }};
using Var{i} = std::variant<{var_types}>;
using Tup{i} = std::tuple<{tup}>;
using Deep{i} = Nest<{k}, {depth}>::type;
template <typename T> struct Box{i} {{
  std::vector<T> items; std::map<std::string, T> by_name;
  template <typename F> auto map(F f) const {{
    std::vector<decltype(f(std::declval<T>()))> out; out.reserve(items.size());
    for (const auto &x : items) out.push_back(f(x));
    return out;
  }}
}};
long run(int n) {{
  std::map<{k}, std::vector<Rec{i}>> m;
  std::unordered_map<std::string, std::set<{k}>> u;
  std::unordered_set<Rec{i}, Hash{i}> hs;
  std::vector<Var{i}> vars;
  Box{i}<Rec{i}> box;
  Deep{i} deep{{}};
  for (int j = 0; j < n; j++) {{
    Rec{i} r{{static_cast<{k}>(j * {i + 3}), {{}}, std::nullopt}};
    m[r.key].push_back(r);
    u[std::to_string(j % {i % 13 + 2})].insert(r.key);
    hs.insert(r);
    box.items.push_back(r);
    box.by_name[std::to_string(j)] = r;
    vars.emplace_back(std::in_place_index<0>);
  }}
  auto keys = box.map([](const Rec{i} &r) {{ return static_cast<long>(r.key); }});
  std::sort(keys.begin(), keys.end(), std::greater<long>());
  long acc = std::accumulate(keys.begin(), keys.end(), 0L);
  for (const auto &v : vars) acc += visit_len(v);
  auto f = [acc](long x) {{ return x + acc % {i + 7}; }};
  acc += apply_twice(f, 3L);
  acc += sum_all(1, 2L, 3u, (short)4, Fib<{fib}>::value % 1000, std::tuple_size_v<Tup{i}>);
  acc += Length<TypeList<{var_types}>>::value + (long)hs.size() + (long)u.size() + (long)sizeof(deep);
  auto p = std::make_shared<Box{i}<Rec{i}>>(box);
  acc += (long)p->items.size();
  return acc;
}}
}}  // namespace m{i}
"""


def main():
    out = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 120
    rng = random.Random(12345)
    parts = [HEADER]
    for i in range(n):
        parts.append(module(i, rng))
    parts.append("long run_all(int n) {\n  long acc = 0;\n")
    for i in range(n):
        parts.append(f"  acc += m{i}::run(n);\n")
    parts.append("  return acc;\n}\n")
    with open(out, "w") as f:
        f.write("".join(parts))


if __name__ == "__main__":
    main()
