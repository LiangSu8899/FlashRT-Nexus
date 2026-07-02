cmake_minimum_required(VERSION 3.16)
project(flashrt_nexus VERSION 0.1.0 LANGUAGES CXX)

# FlashRT Nexus — state-first serving substrate for physical AI.
# Its core is the "capsule" execution-state mechanism: a sovereign,
# zero-dependency C ABI. No third-party deps; the hot path links nothing
# but the C++ runtime. (capsule = the internal mechanism name; see CONTRIBUTING.)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# ---- L1: the capsule core (zero dependency) --------------------------------
add_library(capsule_core STATIC core/src/capsule.cpp)
target_include_directories(capsule_core PUBLIC core/include)

# ---- L2: the host library (standard model-runtime face; mechanism-thin) -----
add_library(capsule_host STATIC host/src/model_runtime.cpp)
target_include_directories(capsule_host PUBLIC host/include)
target_link_libraries(capsule_host PUBLIC capsule_core)

# ---- L2: Nexus schedulers / state services / modes -------------------------
add_library(nexus_l2 STATIC
  nexus/schedulers/stage_dag.cpp
  nexus/schedulers/stage_dag_c.cpp
  nexus/state/graph_store.cpp
  nexus/modes/rtc_action_chunk.cpp
  nexus/modes/rtc_action_chunk_c.cpp)
target_include_directories(nexus_l2
  PUBLIC .
  PRIVATE core/include host/include)
target_link_libraries(nexus_l2 PUBLIC capsule_core capsule_host)

# ---- L0: a host reference backend (for tests / CI; no GPU) ------------------
add_library(capsule_backend_stub STATIC backends/stub/stub_backend.cpp)
target_include_directories(capsule_backend_stub
  PUBLIC backends/stub
  PRIVATE core/include)
target_link_libraries(capsule_backend_stub PUBLIC capsule_core)

# ---- P0 acceptance test -----------------------------------------------------
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE capsule_core capsule_backend_stub)
add_test(NAME core COMMAND test_core)

# Backend conformance contract, run on the host stub (any backend must pass it).
add_executable(test_conformance tests/test_conformance.cpp)
target_link_libraries(test_conformance PRIVATE capsule_core capsule_backend_stub)
add_test(NAME conformance COMMAND test_conformance)

add_executable(test_nexus_l2 tests/test_nexus_l2.cpp)
target_link_libraries(test_nexus_l2 PRIVATE nexus_l2 capsule_backend_stub)
add_test(NAME nexus_l2 COMMAND test_nexus_l2)

# ---- L0: FlashRT exec backend (optional; needs CUDA + libflashrt_exec) ------
# Configure with:
#   -DCAPSULE_BUILD_FLASHRT_BACKEND=ON -DFLASHRT_EXEC_DIR=/path/to/FlashRT/exec
# The GPU smoke test runs in-container on a real GPU; it is built, not auto-run.
option(CAPSULE_BUILD_FLASHRT_BACKEND "Build the FlashRT exec backend" OFF)
if(CAPSULE_BUILD_FLASHRT_BACKEND)
  find_package(CUDAToolkit REQUIRED)
  set(FLASHRT_EXEC_DIR "" CACHE PATH "Path to FlashRT exec/ (include/flashrt/exec.h + a built libflashrt_exec)")
  if(NOT FLASHRT_EXEC_DIR)
    message(FATAL_ERROR "Set -DFLASHRT_EXEC_DIR=<FlashRT>/exec")
  endif()
  find_library(FLASHRT_EXEC_LIB flashrt_exec REQUIRED
    HINTS "${FLASHRT_EXEC_DIR}/build" "${FLASHRT_EXEC_DIR}/build_ctr" "${FLASHRT_EXEC_DIR}")

  # The runtime-export ABI header (flashrt/runtime.h) is consumed as plain
  # data — headers only, nothing extra to link.
  set(FLASHRT_RUNTIME_DIR "${FLASHRT_EXEC_DIR}/../runtime" CACHE PATH
      "Path to FlashRT runtime/ (include/flashrt/runtime.h)")

  add_library(capsule_backend_flashrt STATIC
    backends/flashrt/flashrt_backend.cpp
    backends/flashrt/flashrt_runtime_adapter.cpp
    backends/flashrt/flashrt_model_adapter.cpp)
  target_include_directories(capsule_backend_flashrt
    PUBLIC  backends/flashrt
    PRIVATE core/include host/include "${FLASHRT_EXEC_DIR}/include" "${FLASHRT_RUNTIME_DIR}/include")
  target_link_libraries(capsule_backend_flashrt PUBLIC capsule_core capsule_host CUDA::cudart "${FLASHRT_EXEC_LIB}")

  add_executable(test_flashrt_gpu tests/test_flashrt_gpu.cpp)
  target_include_directories(test_flashrt_gpu PRIVATE "${FLASHRT_EXEC_DIR}/include")
  target_link_libraries(test_flashrt_gpu PRIVATE capsule_core capsule_backend_flashrt CUDA::cudart "${FLASHRT_EXEC_LIB}")

  add_executable(test_runtime_adopt tests/test_runtime_adopt.cpp)
  target_include_directories(test_runtime_adopt
    PRIVATE "${FLASHRT_EXEC_DIR}/include" "${FLASHRT_RUNTIME_DIR}/include")
  target_link_libraries(test_runtime_adopt PRIVATE capsule_core capsule_backend_flashrt CUDA::cudart "${FLASHRT_EXEC_LIB}")

  add_executable(test_model_adopt tests/test_model_adopt.cpp)
  target_include_directories(test_model_adopt
    PRIVATE "${FLASHRT_EXEC_DIR}/include" "${FLASHRT_RUNTIME_DIR}/include")
  target_link_libraries(test_model_adopt PRIVATE capsule_core capsule_host capsule_backend_flashrt CUDA::cudart "${FLASHRT_EXEC_LIB}")

  # Loadable core+backend for in-process hosts (the phase-1 deployment shape:
  # a resident setup process hands its runtime export to Nexus via dlopen/ctypes).
  add_library(capsule_nexus_flashrt SHARED
    core/src/capsule.cpp
    host/src/model_runtime.cpp
    nexus/schedulers/stage_dag.cpp
    nexus/schedulers/stage_dag_c.cpp
    nexus/state/graph_store.cpp
    nexus/modes/rtc_action_chunk.cpp
    nexus/modes/rtc_action_chunk_c.cpp
    backends/flashrt/flashrt_backend.cpp
    backends/flashrt/flashrt_runtime_adapter.cpp
    backends/flashrt/flashrt_model_adapter.cpp)
  target_include_directories(capsule_nexus_flashrt
    PUBLIC  . core/include host/include backends/flashrt
    PRIVATE "${FLASHRT_EXEC_DIR}/include" "${FLASHRT_RUNTIME_DIR}/include")
  target_link_libraries(capsule_nexus_flashrt PUBLIC CUDA::cudart "${FLASHRT_EXEC_LIB}")
endif()
