if(NOT DEFINED SOURCE_DIR OR NOT DEFINED ABI_LIBRARY OR NOT DEFINED NM_TOOL)
  message(FATAL_ERROR "SOURCE_DIR, ABI_LIBRARY and NM_TOOL are required")
endif()

set(generic_sources
  host/src/model_runtime.cpp
  nexus/schedulers/stage_dag.cpp
  backends/flashrt/flashrt_model_abi_adapter.cpp
  backends/flashrt/flashrt_model_adapter.cpp
  backends/flashrt/flashrt_model_extension_internal.h
  backends/flashrt/flashrt_model_loader.cpp)
foreach(relative IN LISTS generic_sources)
  file(READ "${SOURCE_DIR}/${relative}" contents)
  string(TOLOWER "${contents}" contents_lower)
  if(contents_lower MATCHES "pi0|pi05|llama_cpp|jetson_pi")
    message(FATAL_ERROR "model/provider dispatch leaked into ${relative}")
  endif()
endforeach()

file(READ "${SOURCE_DIR}/host/include/capsule/model_runtime.h" public_header)
if(public_header MATCHES "flashrt/|executor_ref|stage_self|FRT_EXT_")
  message(FATAL_ERROR "producer-private capability leaked into public host ABI")
endif()

execute_process(
  COMMAND "${NM_TOOL}" -u "${ABI_LIBRARY}"
  RESULT_VARIABLE nm_rc
  OUTPUT_VARIABLE undefined_symbols
  ERROR_VARIABLE nm_error)
if(NOT nm_rc EQUAL 0)
  message(FATAL_ERROR "nm failed: ${nm_error}")
endif()
string(TOLOWER "${undefined_symbols}" undefined_lower)
if(undefined_lower MATCHES
   "cuda|frt_graph|frt_ctx|frt_buffer|flashrt_adopt_runtime_export|flashrt_backend")
  message(FATAL_ERROR "ABI-only adopter references graph/CUDA symbols")
endif()
