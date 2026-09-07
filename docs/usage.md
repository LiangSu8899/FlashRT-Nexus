# Installation and deployment usage

## 1. Select the runtime

The build helper currently targets Linux shared libraries and requires Python
3.10+, NumPy, a C++17 compiler, CMake, and the standard shell build tools.
Use a dedicated Python environment. Compile and install with the same Python
interpreter; generated wheel tags are interpreter/platform-specific.

| Build | Use | Additional prerequisites |
|---|---|---|
| `--execution-only` | Ecosystem/custom workers and remote clients using C action chunks | No FlashRT headers or CUDA |
| `abi` | Compatible provider-only model-runtime adoption | FlashRT runtime headers and sibling exec headers; no linked FlashRT exec/CUDA backend |
| `exec` | FlashRT GRAPH adoption, including compatible Structures exports | Compatible built FlashRT exec/runtime and their GPU/shared-library dependencies |

ABI-only does not mean every Native model runs without CUDA. The producer owns
its dependencies. Provider-only adoption cannot fabricate GRAPH/SWAP/state
capabilities. Use `exec` for runtimes requiring the FlashRT graph backend.
None of these builds supplies model weights, calibration, processors or drivers.

## 2. Build and install

The reviewed integration baseline is commit
`2a6a877219dcc53b9e0644c7ee13ddc25602e227`, containing merged #17, #16 and #18.
Newer documentation may describe that same baseline. Record the actual source
revision: the package version `0.1.0` alone does not identify its capabilities.

```sh
git clone https://github.com/LiangSu8899/FlashRT-Nexus.git
cd FlashRT-Nexus
git checkout 2a6a877219dcc53b9e0644c7ee13ddc25602e227
python -m pip install setuptools wheel numpy
bash tools/build_native_wheel.sh --execution-only
```

Install the exact wheel printed by the build:

```sh
python -m pip install <generated-wheel-file>
nexus --help
python -m flashrt_nexus.cli --help
```

Replace angle-bracket placeholders with your own inputs. For other variants,
set `FLASHRT_SOURCE` to the compatible producer source checkout and choose one:

```sh
bash tools/build_native_wheel.sh "$FLASHRT_SOURCE" abi
bash tools/build_native_wheel.sh "$FLASHRT_SOURCE" exec
```

The helper uses `--no-build-isolation --no-deps`: install build prerequisites
first. ABI/exec builds require the producer header layout checked by CMake;
exec additionally requires its libraries to be built and discoverable. Changing
variants requires installing the newly generated wheel (use `--force-reinstall
--no-deps` for the same package version if dependencies are already installed).

Library discovery supports an explicit path, `NEXUS_LIB`, installed locations
and source builds. To select the installed execution-only library:

```sh
export NEXUS_LIB="$(python -c 'from flashrt_nexus import find_library; print(find_library(execution_only=True))')"
```

Unset or change this override when switching to graph adoption. Never commit
local asset locations or credentials into manifests.

## 3. Verify before loading a model

From the checkout, install pytest and run:

```sh
python -m pip install pytest
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 python -m pytest -q \
  tests/test_worker.py tests/test_execution_service.py tests/test_serve_shell.py \
  tests/test_package_imports.py tests/test_action_chunk_lifecycle.py
python tests/gate_worker_chunks.py --nexus "$NEXUS_LIB"
```

The worker gate uses a deterministic fixture and prints PASS; it does not access
hardware or qualify a policy. Disabling unrelated pytest plugins keeps this
suite independent of other tools installed in the environment.

## 4. Run locally or serve independently

For local Python execution, implement a provider factory with
`describe / execute / reset / close`, then use
[`ExecutionWorker` and `WorkerActionChunks`](workers.md).
For a model-runtime manifest, use the public
[`EmbeddedSession`](embedded.md). The array convenience host expects its
documented image/state/action contract; a compatible ABI alone does not adapt
arbitrary model inputs or robot action semantics.

For an independent service, create the worker manifest shown in
[execution service](execution_service.md), install its provider in the server
environment and run:

```sh
nexus serve deployment.yaml
curl http://127.0.0.1:8080/healthz
```

The health command assumes the guide's unauthenticated loopback configuration.
It checks service availability, not task success or device readiness.
The client runs `flashrt_nexus.remote:build` inside an `ExecutionWorker`, with
the URL and timeout configuration from that guide, and consumes its chunks
through `WorkerActionChunks`. No separate remote CLI command is implied.

Workers default to `execution_http`. Native model-runtime manifests must set
`serve.transport: execution_http` to use this protocol; their existing default
is the distinct `act_http` API documented in [Serve](serve.md). Do not connect
an execution-service client to an Act API endpoint.

Use compatible Nexus builds in both environments. Remote clients need neither
server weights nor the provider framework. Non-loopback serving requires a
token and protected networking/TLS; do not expose plain HTTP publicly.

## 5. Lifecycle and qualification

One service has one exclusive consumer and one outstanding request/result.
Failed remote execute requires explicit successful reset or a new session,
including a lost ACK response. No automatic retry, reconnect or robot rearming
is performed. Drain/reset is not GPU preemption or a physical emergency stop.
See the [reset distinctions](embedded.md#action-chunk-lifecycle).

The review baseline passed 25 Python tests plus 5 subtests, 12 ABI CTests,
7 execution-only CTests, and a downstream remote simulated-endpoint gate using
an installed Nexus wheel. That installation inherited system dependencies; it
was not a clean dependency-resolution test. GPU policy and physical robot
tests were not rerun in that review. Earlier model evidence does not qualify
new checkpoints, history inputs, RTC adapters or devices automatically.
