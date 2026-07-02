/* nexus/state/graph_store.h — L2 graph-cache budget policy.
 *
 * The backend owns cache mechanics (variant table, evict verbs). Nexus owns
 * policy: per-graph caps, budget checks, and when eviction is safe.
 */
#ifndef NEXUS_STATE_GRAPH_STORE_H
#define NEXUS_STATE_GRAPH_STORE_H

#include "capsule/capsule.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nexus {

class GraphStore {
public:
    using EvictFn = int (*)(cap_backend*, cap_graph, cap_shape_key);
    using EvictLruFn = int (*)(cap_backend*, cap_graph);
    using CountFn = size_t (*)(cap_backend*, cap_graph);

    struct Ops {
        EvictFn evict = nullptr;
        EvictLruFn evict_lru = nullptr;
        CountFn variant_count = nullptr;
    };

    GraphStore(cap_backend* backend, Ops ops);

    int add_graph(std::string name, cap_graph graph, size_t max_variants);
    size_t graph_count() const { return graphs_.size(); }
    size_t variant_count(size_t graph_index) const;
    int enforce_budget(size_t graph_index);
    int evict(size_t graph_index, cap_shape_key key);
    int evict_lru(size_t graph_index);

private:
    struct Entry {
        std::string name;
        cap_graph graph = nullptr;
        size_t max_variants = 0;  /* 0 = unbounded */
    };

    cap_backend* backend_ = nullptr;
    Ops ops_{};
    std::vector<Entry> graphs_;
};

}  // namespace nexus

#endif  /* NEXUS_STATE_GRAPH_STORE_H */
