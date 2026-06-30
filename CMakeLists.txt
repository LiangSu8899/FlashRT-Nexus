cmake_minimum_required(VERSION 3.16)
project(flashrt_nexus LANGUAGES CXX)

# FlashRT Nexus — state-first serving substrate for physical AI.
# Its core is the "capsule" execution-state mechanism: a sovereign,
# zero-dependency C ABI. No third-party deps; the hot path links nothing
# but the C++ runtime. (capsule = the internal mechanism name.)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# ---- L1: the core (zero dependency) ----------------------------------------
add_library(capsule_core STATIC core/src/capsule.cpp)
target_include_directories(capsule_core PUBLIC core/include)

# ---- L0: a host reference backend (for tests / CI; no GPU) ------------------
add_library(capsule_stub_backend STATIC backends/stub/stub_backend.cpp)
target_include_directories(capsule_stub_backend
  PUBLIC backends/stub
  PRIVATE core/include)
target_link_libraries(capsule_stub_backend PUBLIC capsule_core)

# ---- P0 acceptance test -----------------------------------------------------
enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE capsule_core capsule_stub_backend)
add_test(NAME core COMMAND test_core)

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

  add_library(capsule_flashrt_backend STATIC backends/flashrt/flashrt_backend.cpp)
  target_include_directories(capsule_flashrt_backend
    PUBLIC  backends/flashrt
    PRIVATE core/include "${FLASHRT_EXEC_DIR}/include")
  target_link_libraries(capsule_flashrt_backend PUBLIC capsule_core CUDA::cudart "${FLASHRT_EXEC_LIB}")

  add_executable(test_flashrt_gpu tests/test_flashrt_gpu.cpp)
  target_include_directories(test_flashrt_gpu PRIVATE "${FLASHRT_EXEC_DIR}/include")
  target_link_libraries(test_flashrt_gpu PRIVATE capsule_core capsule_flashrt_backend CUDA::cudart "${FLASHRT_EXEC_LIB}")
endif()
