/* capsule.cpp — reference implementation of the Capsule core C ABI.
 *
 * Zero third-party dependencies (C++ stdlib only). The core orchestrates over
 * the backend vtable: it allocates/copies named regions (Capsule) and issues
 * replays (Drive). It owns no loop, no thread, and takes no lock. Hot-path
 * verbs (cap_fire/cap_swap/cap_sync/cap_capsule_ready) perform no allocation.
 */
#include "capsule/capsule.h"

#include <vector>
#include <cstring>
#include <cstdint>
#include <new>

namespace {

constexpr uint32_t CAP_MAGIC = 0x43415053u; /* 'CAPS' */
constexpr uint32_t CAP_BLOB_VERSION = 1u;

struct RegionStore {
    cap_buffer backing;      /* core-owned copy of the state             */
    cap_buffer origin_buf;   /* live buffer it came from (NULL if loaded) */
    size_t     origin_off;
    size_t     bytes;
};

inline int space_of_tier(int tier) {
    return (tier == CAP_TIER_GPU) ? CAP_DEV : CAP_HOST;
}

/* map a backend status (0 == ok) onto cap_status */
inline int be_ok(int rc) { return rc == 0 ? CAP_OK : CAP_ERR_BACKEND; }

} /* namespace */

struct cap_ctx_s {
    cap_backend be;
};

struct cap_capsule_s {
    std::vector<RegionStore> regions;
    std::vector<unsigned char> meta;
    uint64_t fingerprint = 0;
    int      tier = CAP_TIER_GPU;
    cap_event ready_ev = nullptr;
    bool     has_origin = false;
};

/* ---- context ------------------------------------------------------------- */

cap_ctx cap_ctx_create(const cap_backend* backend) {
    if (!backend) return nullptr;
    if (backend->abi_version != CAP_ABI_VERSION) return nullptr;
    if (backend->struct_size != (uint32_t)sizeof(cap_backend)) return nullptr;
    cap_ctx c = new (std::nothrow) cap_ctx_s();
    if (!c) return nullptr;
    c->be = *backend;
    return c;
}

void cap_ctx_destroy(cap_ctx c) { delete c; }

uint64_t cap_ctx_fingerprint(cap_ctx c) {
    return c ? c->be.fingerprint(c->be.self) : 0;
}

/* ---- Capsule ------------------------------------------------------------- */

cap_capsule cap_snapshot(cap_ctx c, const cap_boundary* b, int tier, int stream) {
    if (!c || !b || b->n_regions < 0) return nullptr;
    cap_capsule cap = new (std::nothrow) cap_capsule_s();
    if (!cap) return nullptr;
    cap->fingerprint = c->be.fingerprint(c->be.self);
    cap->tier = tier;
    cap->has_origin = true;
    if (b->meta && b->meta_len)
        cap->meta.assign((const unsigned char*)b->meta,
                         (const unsigned char*)b->meta + b->meta_len);

    const int space = space_of_tier(tier);
    for (int i = 0; i < b->n_regions; ++i) {
        const cap_region& r = b->regions[i];
        cap_buffer backing = c->be.buffer_alloc(c->be.self, "capsule", r.bytes, space);
        if (!backing) { cap_capsule_drop(c, cap); return nullptr; }
        if (c->be.buffer_copy(c->be.self, backing, 0, r.buf, r.off, r.bytes, stream) != 0) {
            c->be.buffer_free(c->be.self, backing);
            cap_capsule_drop(c, cap);
            return nullptr;
        }
        cap->regions.push_back(RegionStore{backing, r.buf, r.off, r.bytes});
    }
    cap->ready_ev = c->be.event(c->be.self);
    if (cap->ready_ev) c->be.event_record(c->be.self, cap->ready_ev, stream);
    return cap;
}

int cap_capsule_ready(cap_ctx c, cap_capsule cap) {
    if (!c || !cap) return CAP_ERR_ARG;
    if (!cap->ready_ev) return CAP_OK;
    return c->be.event_query(c->be.self, cap->ready_ev);
}

int cap_restore(cap_ctx c, cap_capsule cap, int stream) {
    if (!c || !cap) return CAP_ERR_ARG;
    if (!cap->has_origin) return CAP_ERR_ARG;     /* loaded capsule: use restore_into */
    if (cap->fingerprint != c->be.fingerprint(c->be.self)) return CAP_ERR_FINGERPRINT;
    for (const RegionStore& s : cap->regions) {
        int rc = c->be.buffer_copy(c->be.self, s.origin_buf, s.origin_off,
                                   s.backing, 0, s.bytes, stream);
        if (rc != 0) return CAP_ERR_BACKEND;
    }
    return CAP_OK;
}

int cap_restore_into(cap_ctx c, cap_capsule cap, const cap_region* dst, int n, int stream) {
    if (!c || !cap || !dst) return CAP_ERR_ARG;
    if (n != (int)cap->regions.size()) return CAP_ERR_ARG;
    if (cap->fingerprint != c->be.fingerprint(c->be.self)) return CAP_ERR_FINGERPRINT;
    for (int i = 0; i < n; ++i) {
        if (dst[i].bytes < cap->regions[i].bytes) return CAP_ERR_ARG;
        int rc = c->be.buffer_copy(c->be.self, dst[i].buf, dst[i].off,
                                   cap->regions[i].backing, 0, cap->regions[i].bytes, stream);
        if (rc != 0) return CAP_ERR_BACKEND;
    }
    return CAP_OK;
}

int cap_regions(cap_ctx c, cap_capsule cap, cap_region_view* out, int* n) {
    if (!c || !cap || !n) return CAP_ERR_ARG;
    const int cnt = (int)cap->regions.size();
    if (!out) { *n = cnt; return CAP_OK; }
    const int lim = (*n < cnt) ? *n : cnt;
    for (int i = 0; i < lim; ++i) {
        out[i].ptr   = c->be.buffer_ptr(c->be.self, cap->regions[i].backing);
        out[i].bytes = cap->regions[i].bytes;
    }
    *n = cnt;
    return CAP_OK;
}

int cap_tier_move(cap_ctx c, cap_capsule cap, int to_tier, int stream) {
    if (!c || !cap) return CAP_ERR_ARG;
    if (to_tier == cap->tier) return CAP_OK;
    const int space = space_of_tier(to_tier);
    for (RegionStore& s : cap->regions) {
        cap_buffer nb = c->be.buffer_alloc(c->be.self, "capsule", s.bytes, space);
        if (!nb) return CAP_ERR_NOMEM;
        if (c->be.buffer_copy(c->be.self, nb, 0, s.backing, 0, s.bytes, stream) != 0) {
            c->be.buffer_free(c->be.self, nb);
            return CAP_ERR_BACKEND;
        }
        c->be.sync(c->be.self, stream);               /* ensure copy done before freeing old */
        c->be.buffer_free(c->be.self, s.backing);
        s.backing = nb;
    }
    cap->tier = to_tier;
    return CAP_OK;
}

/* blob layout (native-endian; capsules are deployment-bound by design):
 *   u32 magic, u32 version, u64 fingerprint, u32 tier, u32 n_regions,
 *   u64 meta_len, u64 region_bytes[n], meta[meta_len], region_data... */
int cap_serialize(cap_ctx c, cap_capsule cap, void* out, size_t* len) {
    if (!c || !cap || !len) return CAP_ERR_ARG;
    const uint32_t n = (uint32_t)cap->regions.size();
    const uint64_t meta_len = (uint64_t)cap->meta.size();
    size_t total = 4 + 4 + 8 + 4 + 4 + 8 + (size_t)n * 8 + (size_t)meta_len;
    for (const RegionStore& s : cap->regions) total += s.bytes;
    if (!out) { *len = total; return CAP_OK; }
    if (*len < total) return CAP_ERR_ARG;

    unsigned char* p = (unsigned char*)out;
    auto put32 = [&](uint32_t v){ memcpy(p, &v, 4); p += 4; };
    auto put64 = [&](uint64_t v){ memcpy(p, &v, 8); p += 8; };
    put32(CAP_MAGIC); put32(CAP_BLOB_VERSION); put64(cap->fingerprint);
    put32((uint32_t)cap->tier); put32(n); put64(meta_len);
    for (const RegionStore& s : cap->regions) put64((uint64_t)s.bytes);
    if (meta_len) { memcpy(p, cap->meta.data(), cap->meta.size()); p += cap->meta.size(); }
    for (const RegionStore& s : cap->regions) {
        int rc = c->be.buffer_download(c->be.self, s.backing, 0, p, s.bytes, 0);
        if (rc != 0) return CAP_ERR_BACKEND;
        p += s.bytes;
    }
    c->be.sync(c->be.self, 0);
    *len = total;
    return CAP_OK;
}

cap_capsule cap_load(cap_ctx c, const void* blob, size_t len) {
    if (!c || !blob || len < 32) return nullptr;
    const unsigned char* p = (const unsigned char*)blob;
    const unsigned char* end = p + len;
    auto get32 = [&](uint32_t& v){ memcpy(&v, p, 4); p += 4; };
    auto get64 = [&](uint64_t& v){ memcpy(&v, p, 8); p += 8; };
    uint32_t magic, ver, n, tier; uint64_t fp, meta_len;
    get32(magic); get32(ver); get64(fp); get32(tier); get32(n); get64(meta_len);
    if (magic != CAP_MAGIC || ver != CAP_BLOB_VERSION) return nullptr;
    if (fp != c->be.fingerprint(c->be.self)) return nullptr;   /* fingerprint guard */

    if (p + (size_t)n * 8 + meta_len > end) return nullptr;
    std::vector<uint64_t> sizes(n);
    for (uint32_t i = 0; i < n; ++i) get64(sizes[i]);

    cap_capsule cap = new (std::nothrow) cap_capsule_s();
    if (!cap) return nullptr;
    cap->fingerprint = fp;
    cap->tier = CAP_TIER_HOST;
    cap->has_origin = false;
    if (meta_len) { cap->meta.assign(p, p + meta_len); p += meta_len; }

    for (uint32_t i = 0; i < n; ++i) {
        if (p + sizes[i] > end) { cap_capsule_drop(c, cap); return nullptr; }
        cap_buffer backing = c->be.buffer_alloc(c->be.self, "capsule", sizes[i], CAP_HOST);
        if (!backing) { cap_capsule_drop(c, cap); return nullptr; }
        if (c->be.buffer_upload(c->be.self, backing, 0, p, sizes[i], 0) != 0) {
            c->be.buffer_free(c->be.self, backing);
            cap_capsule_drop(c, cap);
            return nullptr;
        }
        p += sizes[i];
        cap->regions.push_back(RegionStore{backing, nullptr, 0, (size_t)sizes[i]});
    }
    c->be.sync(c->be.self, 0);
    cap->ready_ev = c->be.event(c->be.self);
    if (cap->ready_ev) c->be.event_record(c->be.self, cap->ready_ev, 0);
    return cap;
}

void cap_capsule_drop(cap_ctx c, cap_capsule cap) {
    if (!c || !cap) return;
    for (RegionStore& s : cap->regions)
        if (s.backing) c->be.buffer_free(c->be.self, s.backing);
    if (cap->ready_ev) c->be.event_free(c->be.self, cap->ready_ev);
    delete cap;
}

/* ---- Drive --------------------------------------------------------------- */

int cap_fire(cap_ctx c, const cap_stage* st) {
    if (!c || !st) return CAP_ERR_ARG;
    if (!c->be.graph_has(c->be.self, st->graph, st->key)) return CAP_ERR_ARG;
    return be_ok(c->be.graph_replay(c->be.self, st->graph, st->key, st->stream));
}

int cap_drive_tick(cap_ctx c, const cap_schedule* sc, uint64_t clock, int* failed_stage) {
    if (failed_stage) *failed_stage = -1;
    if (!c || !sc) return CAP_ERR_ARG;
    std::vector<cap_event> evs(sc->n_stages, nullptr);
    int status = CAP_OK;
    for (int i = 0; i < sc->n_stages; ++i) {
        const cap_stage& st = sc->stages[i];
        if (st.trigger != CAP_EVERY) continue;                 /* tick fires EVERY-cadence stages only */
        const bool due = (st.cadence_den <= 0) ||
                         ((clock % (uint64_t)st.cadence_den) < (uint64_t)st.cadence_num);
        if (!due) continue;
        for (int d = 0; d < sc->n_deps; ++d) {                 /* honor cross-stage event deps */
            if (sc->deps[d][0] == i) {
                int j = sc->deps[d][1];
                if (j >= 0 && j < sc->n_stages && evs[j])
                    c->be.stream_wait(c->be.self, st.stream, evs[j]);
            }
        }
        if (c->be.graph_replay(c->be.self, st.graph, st.key, st.stream) != 0) {
            if (failed_stage) *failed_stage = i;
            status = CAP_ERR_BACKEND;
            break;
        }
        evs[i] = c->be.event(c->be.self);
        if (evs[i]) c->be.event_record(c->be.self, evs[i], st.stream);
    }
    for (cap_event e : evs) if (e) c->be.event_free(c->be.self, e);
    return status;
}

int cap_swap(cap_ctx c, cap_buffer dst, const void* src, size_t n, int stream) {
    if (!c || !dst || !src) return CAP_ERR_ARG;
    return be_ok(c->be.buffer_upload(c->be.self, dst, 0, src, n, stream));
}

int cap_sync(cap_ctx c, int stream) {
    if (!c) return CAP_ERR_ARG;
    return be_ok(c->be.sync(c->be.self, stream));
}
