#include "capsule/model_runtime.h"
#include "flashrt_model_loader.h"

#include <dlfcn.h>

#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    flashrt_loaded_model* first = nullptr;
    flashrt_loaded_model* second = nullptr;
    cap_model_runtime* first_model = nullptr;
    cap_model_runtime* second_model = nullptr;
    if (flashrt_loaded_model_open(argv[1], "{\"fixture\":true}", &first,
                                  &first_model) != CAP_OK ||
        flashrt_loaded_model_open(argv[1], "{\"fixture\":true}", &second,
                                  &second_model) != CAP_OK) return 3;
    if (cap_model_tick(nullptr, first_model) != CAP_OK ||
        cap_model_tick(nullptr, second_model) != CAP_OK) return 4;
    int calls = 0;
    uint64_t written = 0;
    if (cap_model_get_output(second_model, 0, &calls, sizeof(calls),
                             &written, -1) != CAP_OK || calls != 1 ||
        written != sizeof(calls)) return 5;

    flashrt_loaded_model_close(first);
    if (cap_model_tick(nullptr, second_model) != CAP_OK) return 6;
    void* resident = dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD);
    if (!resident) return 7;
    dlclose(resident);
    flashrt_loaded_model_close(second);
    if (dlopen(argv[1], RTLD_NOW | RTLD_NOLOAD)) return 8;

    flashrt_loaded_model* rejected = nullptr;
    cap_model_runtime* rejected_model = nullptr;
    if (flashrt_loaded_model_open(argv[1], "{}", &rejected,
                                  &rejected_model) != CAP_ERR_BACKEND ||
        rejected || rejected_model ||
        !flashrt_model_loader_last_error()[0]) return 9;
    std::puts("PASS - provider DSO lifetime is bound to the adopted runtime");
    return 0;
}
