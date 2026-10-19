// tests/data/templates.cpp -- template-heavy C++ for preload_smoke.sh: the
// compiler (cc1plus) allocates heavily instantiating these; the program's
// output is diffed between allocators.
#include <algorithm>
#include <array>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

template <int N> struct Fib { static constexpr long value = Fib<N - 1>::value + Fib<N - 2>::value; };
template <> struct Fib<1> { static constexpr long value = 1; };
template <> struct Fib<0> { static constexpr long value = 0; };

template <typename... Ts> struct TypeList {};
template <typename L> struct Size;
template <typename... Ts> struct Size<TypeList<Ts...>> { static constexpr size_t value = sizeof...(Ts); };

template <size_t... Is> constexpr auto squares(std::index_sequence<Is...>) { return std::array<size_t, sizeof...(Is)>{(Is * Is)...}; }

template <typename T, int Depth> struct Nest {
    std::vector<Nest<T, Depth - 1>> kids;
    T val{};
    T sum() const { T s = val; for (auto &k : kids) s += k.sum(); return s; }
};
template <typename T> struct Nest<T, 0> { T val{}; T sum() const { return val; } };

template <typename T, int D> Nest<T, D> build(int fan, T v)
{
    Nest<T, D> n;
    n.val = v;
    if constexpr (D > 0)
        for (int i = 0; i < fan; i++) n.kids.push_back(build<T, D - 1>(fan, v + (T)i));
    return n;
}

template <typename K, typename V> std::map<K, std::vector<V>> group(const std::vector<std::pair<K, V>> &in)
{
    std::map<K, std::vector<V>> out;
    for (auto &[k, v] : in) out[k].push_back(v);
    return out;
}

using Var = std::variant<int, double, std::string, std::vector<int>>;
struct Visitor {
    std::string operator()(int i) const { return "i" + std::to_string(i); }
    std::string operator()(double d) const { return "d" + std::to_string((long)(d * 10)); }
    std::string operator()(const std::string &s) const { return "s" + s; }
    std::string operator()(const std::vector<int> &v) const { return "v" + std::to_string(v.size()); }
};

template <typename F, typename... Args> auto apply_all(F f, Args &&...args) { return std::make_tuple(f(std::forward<Args>(args))...); }

#define INST(N) template struct Nest<long, N>;
INST(1) INST(2) INST(3) INST(4) INST(5)

int main()
{
    std::printf("fib %ld %ld\n", Fib<40>::value, Fib<60>::value);
    std::printf("size %zu\n", Size<TypeList<int, char, double, std::string, std::vector<int>>>::value);
    auto sq = squares(std::make_index_sequence<64>{});
    std::printf("squares %zu\n", std::accumulate(sq.begin(), sq.end(), (size_t)0));
    auto tree = build<long, 5>(6, 1);
    std::printf("tree %ld\n", tree.sum());
    std::vector<std::pair<std::string, int>> in;
    for (int i = 0; i < 50000; i++) in.emplace_back("g" + std::to_string(i % 101), i);
    auto g = group(in);
    long acc = 0;
    for (auto &[k, v] : g) acc += (long)k.size() * (long)v.size() + v.back();
    std::printf("group %zu %ld\n", g.size(), acc);
    std::vector<Var> vars;
    for (int i = 0; i < 1000; i++) {
        switch (i % 4) {
        case 0: vars.emplace_back(i); break;
        case 1: vars.emplace_back(i * 0.5); break;
        case 2: vars.emplace_back(std::string(i % 13, 'z')); break;
        default: vars.emplace_back(std::vector<int>((size_t)(i % 7), i)); break;
        }
    }
    size_t tot = 0;
    for (auto &v : vars) tot += std::visit(Visitor{}, v).size();
    std::printf("variant %zu\n", tot);
    auto t = apply_all([](auto x) { return sizeof(x); }, 1, 2.0, 'c', std::string("s"));
    std::printf("tuple %zu %zu %zu %zu\n", std::get<0>(t), std::get<1>(t), std::get<2>(t), std::get<3>(t));
    std::unordered_map<std::string, std::shared_ptr<std::set<int>>> um;
    for (int i = 0; i < 20000; i++) {
        auto &p = um["u" + std::to_string(i % 300)];
        if (!p) p = std::make_shared<std::set<int>>();
        p->insert(i % 1000);
    }
    size_t setsum = 0;
    for (auto &[k, v] : um) setsum += v->size();
    std::printf("umap %zu %zu\n", um.size(), setsum);
    std::optional<std::function<int(int)>> f = [](int x) { return x * 3; };
    std::printf("opt %d\n", (*f)(14));
    return 0;
}
