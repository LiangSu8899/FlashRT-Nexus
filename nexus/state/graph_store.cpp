#include "nexus/state/graph_store.h"

namespace nexus {

GraphStore::GraphStore(cap_backend* backend, Ops ops)
    : backend_(backend), ops_(ops) {}

int GraphStore::add_graph(std::string name, cap_graph graph,
                          size_t max_variants) {
    if (!backend_ || !graph || name.empty()) return CAP_ERR_ARG;
    graphs_.push_back(Entry{std::move(name), graph, max_variants});
    return CAP_OK;
}

size_t GraphStore::variant_count(size_t graph_index) const {
    if (graph_index >= graphs_.size() || !ops_.variant_count) return 0;
    return ops_.variant_count(backend_, graphs_[graph_index].graph);
}

int GraphStore::enforce_budget(size_t graph_index) {
    if (graph_index >= graphs_.size()) return CAP_ERR_ARG;
    const Entry& e = graphs_[graph_index];
    if (!e.max_variants) return CAP_OK;
    if (!ops_.variant_count || !ops_.evict_lru) return CAP_ERR_ARG;
    while (ops_.variant_count(backend_, e.graph) > e.max_variants) {
        int rc = ops_.evict_lru(backend_, e.graph);
        if (rc != CAP_OK) return rc;
    }
    return CAP_OK;
}

int GraphStore::evict(size_t graph_index, cap_shape_key key) {
    if (graph_index >= graphs_.size() || !ops_.evict) return CAP_ERR_ARG;
    return ops_.evict(backend_, graphs_[graph_index].graph, key);
}

int GraphStore::evict_lru(size_t graph_index) {
    if (graph_index >= graphs_.size() || !ops_.evict_lru) return CAP_ERR_ARG;
    return ops_.evict_lru(backend_, graphs_[graph_index].graph);
}

}  // namespace nexus
