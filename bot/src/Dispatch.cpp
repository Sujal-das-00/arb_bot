#include "Dispatch.h"

#include <unordered_set>

namespace dispatch {

namespace {
// Append `sym` to `out`/`seen` if we haven't emitted it yet. Keeps first-seen
// order (deterministic logs) while collapsing common symbols to one entry.
void push_unique(const std::string& sym,
                 std::vector<std::string>& out,
                 std::unordered_set<std::string>& seen) {
    if (seen.insert(sym).second) out.push_back(sym);
}
} // namespace

std::vector<std::string> collect_symbols(const std::vector<TriangleDef>& tris) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    out.reserve(tris.size() * 3);  // heuristic; might resize
    for (const auto& t : tris) {
        push_unique(t.outer_usdt, out, seen);
        push_unique(t.outer_btc,  out, seen);
        push_unique(t.btc_usdt,   out, seen);
    }
    return out;
}

std::unordered_map<std::string, std::vector<std::size_t>>
build_reverse_index(const std::vector<TriangleDef>& tris) {
    std::unordered_map<std::string, std::vector<std::size_t>> index;
    for (std::size_t i = 0; i < tris.size(); ++i) {
        const auto& t = tris[i];
        index[t.outer_usdt].push_back(i);
        index[t.outer_btc].push_back(i);
        index[t.btc_usdt].push_back(i);
    }
    return index;
}

} // namespace dispatch
