#include "flashrt_model_loader.h"

#if defined(FLASHRT_MODEL_LOADER_ABI_ONLY)
#include "flashrt_model_abi_adapter.h"
#else
#include "flashrt_model_adapter.h"
#endif

#include "flashrt/model_runtime.h"

#include <dlfcn.h>

#include <cstring>
#include <new>
#include <string>

struct flashrt_loaded_model {
    void* dso = nullptr;
    cap_model_runtime* model = nullptr;
};

namespace {

thread_local std::string loader_error;

void set_dl_error(const char* prefix, const char* detail) {
    loader_error = prefix;
    if (detail && detail[0]) {
        loader_error += ": ";
        loader_error += detail;
    }
}

void close_adopted(cap_model_runtime* model) {
#if defined(FLASHRT_MODEL_LOADER_ABI_ONLY)
    flashrt_model_abi_close(model);
#else
    flashrt_model_close(model);
#endif
}

int adopt(const frt_model_runtime_v1* runtime, cap_model_runtime** out) {
#if defined(FLASHRT_MODEL_LOADER_ABI_ONLY)
    return flashrt_adopt_model_runtime_abi(runtime, out);
#else
    return flashrt_adopt_model_runtime(runtime, out);
#endif
}

}  // namespace

extern "C" int flashrt_loaded_model_open(
        const char* provider_dso, const char* config_json,
        flashrt_loaded_model** out_loader, cap_model_runtime** out_model) {
    if (out_loader) *out_loader = nullptr;
    if (out_model) *out_model = nullptr;
    loader_error.clear();
    if (!provider_dso || !provider_dso[0] || !out_loader || !out_model) {
        loader_error = "invalid loader arguments";
        return CAP_ERR_ARG;
    }

    void* dso = dlopen(provider_dso, RTLD_NOW | RTLD_LOCAL);
    if (!dso) {
        set_dl_error("provider load failed", dlerror());
        return CAP_ERR_BACKEND;
    }
    dlerror();
    void* symbol = dlsym(dso, FRT_MODEL_RUNTIME_OPEN_V1_SYMBOL);
    const char* symbol_error = dlerror();
    if (!symbol || symbol_error) {
        set_dl_error("provider factory lookup failed", symbol_error);
        dlclose(dso);
        return CAP_ERR_FORMAT;
    }
    frt_model_runtime_open_v1_fn open_v1 = nullptr;
    static_assert(sizeof(open_v1) == sizeof(symbol),
                  "function and data pointers must have equal size");
    std::memcpy(&open_v1, &symbol, sizeof(open_v1));

    frt_model_runtime_v1* runtime = nullptr;
    const int open_rc = open_v1(config_json, &runtime);
    if (open_rc != 0 || !runtime || !runtime->release) {
        loader_error = "provider open failed";
        if (runtime && runtime->release) runtime->release(runtime->owner);
        dlclose(dso);
        return CAP_ERR_BACKEND;
    }

    cap_model_runtime* adopted = nullptr;
    const int adopt_rc = adopt(runtime, &adopted);
    runtime->release(runtime->owner);  /* adopter owns its retained reference */
    if (adopt_rc != 0 || !adopted) {
        loader_error = "provider runtime adoption failed";
        dlclose(dso);
        return CAP_ERR_FORMAT;
    }

    auto* loader = new (std::nothrow) flashrt_loaded_model{dso, adopted};
    if (!loader) {
        close_adopted(adopted);
        dlclose(dso);
        loader_error = "loader allocation failed";
        return CAP_ERR_NOMEM;
    }
    *out_loader = loader;
    *out_model = adopted;
    return CAP_OK;
}

extern "C" void flashrt_loaded_model_close(flashrt_loaded_model* loader) {
    if (!loader) return;
    if (loader->model) close_adopted(loader->model);
    if (loader->dso) dlclose(loader->dso);
    delete loader;
}

extern "C" const char* flashrt_model_loader_last_error(void) {
    return loader_error.c_str();
}
