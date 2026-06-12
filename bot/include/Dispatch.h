#pragma once

#include "TriangleDef.h"

#include <string>
#include <vector>
#include <unordered_map>

// Routing helpers for watching N triangles over one feed. Both functions are
// pure and build-once: they run at startup, never on the hot path, so they are
// trivially unit-testable without any network or io_context.
namespace dispatch {

// Every distinct stream symbol referenced by any triangle, deduplicated. The
// shared bridge (BTCUSDT) collapses to a single entry no matter how many
// triangles use it, so 10 triangles sharing BTCUSDT yield 21 symbols, not 30.
// Order is deterministic (first-seen) so logs and subscriptions are stable.
std::vector<std::string> collect_symbols(const std::vector<TriangleDef>& tris);

// Reverse index: symbol -> indices into `tris` of every triangle that uses it.
// On each parsed message the hot path does one lookup here and re-evaluates
// only the triangles in the returned bucket. A symbol no triangle references
// (e.g. an unexpected stream) is simply absent, so it triggers zero work.
std::unordered_map<std::string, std::vector<std::size_t>>
build_reverse_index(const std::vector<TriangleDef>& tris);

} // namespace dispatch
