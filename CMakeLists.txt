cmake_minimum_required(VERSION 3.16)
project(capsule LANGUAGES CXX)

# The Capsule core: sovereign, zero-dependency C ABI inference definition.
# No third-party deps. The hot path links nothing but the C++ runtime.
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
