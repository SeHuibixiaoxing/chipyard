cmake_minimum_required(VERSION 3.5.0)
project(MudnacSim)

set(CMAKE_CXX_STANDARD 26)

# message(STATUS "AddressSanitizer enabled: adding -fsanitize=address flags")
# # Prefer lower optimization to make ASAN reports clearer
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O1 -fno-omit-frame-pointer -fsanitize=address")
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O1 -fno-omit-frame-pointer -fsanitize=address")
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
# set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")

set(CMAKE_PREFIX_PATH "$ENV{CONDA_PREFIX}/lib;$ENV{CONDA_PREFIX}/lib64;${CMAKE_PREFIX_PATH}")
set(CMAKE_LIBRARY_PATH "$ENV{CONDA_PREFIX}/lib;$ENV{CONDA_PREFIX}/lib64;${CMAKE_LIBRARY_PATH}")



include_directories(
        "${PROJECT_SOURCE_DIR}/DRAMsim3/src"
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/src/runtime"
        "${PROJECT_SOURCE_DIR}/src/noc"
        "${PROJECT_SOURCE_DIR}/include"
        "$ENV{CONDA_PREFIX}/include"
)

link_libraries(
        "${PROJECT_SOURCE_DIR}/DRAMsim3/libdramsim3.so"
)

find_library(LIB_YAML_CPP yaml-cpp)
link_libraries(${LIB_YAML_CPP})

# nlohmann_json is header-only on many systems; avoid requiring a library.
# If a CMake package exists, link its target for usage requirements; otherwise skip linking.
find_package(nlohmann_json QUIET CONFIG)
if (nlohmann_json_FOUND)
        link_libraries(nlohmann_json::nlohmann_json)
endif()

link_libraries(pthread)

find_package(cxxopts REQUIRED)

add_library(mudnacsim STATIC
        src/tools.h
        src/xbar.h
        src/mem_ctrl.cpp
        src/mem_ctrl.h
        src/cache.cpp
        src/cache.h
        src/base_accel.cpp
        src/base_accel.h
        src/tensor.cpp
        src/tensor.h
        src/cache_accel.cpp
        src/cache_accel.h
        src/spm.cpp
        src/spm.h
        src/spm_accel.cpp
        src/spm_accel.h
        src/cache_system.h
        src/spm_system.h
        src/runtime/runtime.h
        src/runtime/runtime.cpp
        src/runtime/layer_resadd.h
        src/runtime/layer_conv.h
        src/base_system.h
        src/runtime/mapping.h
        src/runtime/mapping.cpp
        include/mudnacsim.h
        src/runtime/thread_task_queue.h
        src/runtime/global_variables.cpp
        src/runtime/global_variables.h
        src/runtime/model_loader.h
        src/runtime/model_loader.cpp
        src/runtime/spm_manager.cpp
        src/runtime/spm_manager.h
        src/runtime/pipeline_buffer.cpp
        src/runtime/pipeline_buffer.h
        src/runtime/pipeline_stage.cpp
        src/runtime/pipeline_stage.h
        src/runtime/pipeline.cpp
        src/runtime/pipeline.h
        src/system_config.h
        src/runtime/thread_qos.h
        src/runtime/thread_run_segment.cpp
        src/runtime/thread_run_segment.h
        src/runtime/thread_run_pipeline.cpp
        src/runtime/thread_run_pipeline.h
        src/runtime/thread_run_layer.h
        src/runtime/thread_run_model.h
        src/noc/router.cpp
        src/noc/router.h
        src/noc/flit.h
        src/runtime/loopnest.h
        src/noc/net_int.h
        src/noc/cache_noc_system.h
        src/noc/noc.h
        src/noc/spm_noc_system.h
        src/noc/base_noc_system.h
        src/arg_parser.h
        src/runtime/accelerator_manager.cpp
        src/runtime/accelerator_manager.h
        src/runtime/thread_qos_pipeline.cpp
        src/runtime/thread_qos_pipeline.h
        src/runtime/qos_scheduler_new.cpp
        src/runtime/thread_qos_pipeline_new.cpp
        src/runtime/thread_qos_pipeline_new.h
)

link_libraries(mudnacsim)

# test
# add_executable(test_libdramsim3 test/test_libdramsim3.cpp)
# add_executable(test_port_id test/test_port_id.cpp)
# add_executable(test_mem_ctrl test/test_mem_ctrl.cpp)
# add_executable(test_xbar test/test_xbar.cpp)
# add_executable(test_arbitor test/test_arbitor.cpp)
# add_executable(test_cache test/test_cache.cpp)
# add_executable(test_cache_memctrl test/test_cache_memctrl.cpp)
# add_executable(test_base_accel test/test_base_accel.cpp)
# add_executable(test_spm test/test_spm.cpp)
# add_executable(test_spm_acc test/test_spm_acc.cpp)
# add_executable(test_spm_acc_one_cmd test/test_spm_acc_one_cmd.cpp)
# add_executable(test_tensor test/test_tensor.cpp)
# add_executable(test_LoopNest test/test_loopnest.cpp)
# add_executable(test_yaml test/test_yaml.cpp)
# add_executable(test_router test/noc/test_router.cpp)
# add_executable(test_model_loader test/test_model_loader.cpp)
# add_executable(test_noc_system test/noc/test_noc_system.cpp)
# add_executable(test_net_int test/noc/test_net_int.cpp)
# add_executable(test_noc test/noc/test_noc.cpp)

# expr
# add_executable(expr_multi_models expr/expr_multi_models.cpp)
# add_executable(expr_single_layer expr/expr_single_layer.cpp)
# add_executable(expr_single_model expr/expr_single_model.cpp)
# add_executable(expr_mixed_models expr/expr_mixed_models.cpp)
# add_executable(expr_profiling expr/expr_profiling.cpp)
# add_executable(expr_mixed_models2 expr/expr_mixed_models2.cpp)
# add_executable(expr_qos expr/expr_qos.cpp)

# add_executable(expr_profiling2 expr/expr_profiling2.cpp)
# add_executable(expr_qos2 expr/expr_qos2.cpp)
# add_executable(expr_mixed_models3 expr/expr_mixed_models3.cpp)
add_executable(expr_single_pipeline expr/expr_single_pipeline.cpp)
# add_executable(expr_multi_pipeline expr/expr_multi_pipeline.cpp) 
# add_executable(expr_qos_pipeline expr/expr_qos_pipeline.cpp)
# add_executable(expr_qps_pipeline_new expr/expr_qps_pipeline_new.cpp)
