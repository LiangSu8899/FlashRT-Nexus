/* test_conformance.cpp — run the backend conformance contract on the host stub
 * backend (zero dependency, CI-friendly). Proves the contract any backend must
 * meet; real backends run the same suite on their hardware. */
#include "backend_conformance.h"
#include "stub_backend.h"

int main() {
    cap_backend be;         stub_backend_init(&be, 0xC0FFEEull);
    cap_backend be_foreign; stub_backend_init(&be_foreign, 0xDEADBEEFull);  /* different fingerprint */

    int rc = run_backend_conformance(&be, &be_foreign);

    stub_backend_fini(&be_foreign);
    stub_backend_fini(&be);
    return rc;
}
