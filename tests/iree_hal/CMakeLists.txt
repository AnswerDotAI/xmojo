cmake_minimum_required(VERSION 3.25)

project(xmojo_iree_hal_probe LANGUAGES C CXX OBJC OBJCXX)

enable_testing()

if(NOT APPLE)
  message(FATAL_ERROR "The initial xmojo GPU runtime supports Metal on macOS")
endif()

get_filename_component(XMOJO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
get_filename_component(XMOJO_DEFAULT_IREE_ROOT "${XMOJO_ROOT}/.." ABSOLUTE)
set(XMOJO_IREE_ROOT "${XMOJO_DEFAULT_IREE_ROOT}/iree" CACHE PATH "Path to the sibling IREE checkout")
if(NOT EXISTS "${XMOJO_IREE_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR "IREE checkout not found at ${XMOJO_IREE_ROOT}")
endif()

set(IREE_BUILD_COMPILER OFF CACHE BOOL "" FORCE)
set(IREE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IREE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(IREE_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(IREE_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
set(IREE_BUILD_BINDINGS_TFLITE OFF CACHE BOOL "" FORCE)
set(IREE_BUILD_BINDINGS_TFLITE_JAVA OFF CACHE BOOL "" FORCE)
set(IREE_HAL_DRIVER_DEFAULTS OFF CACHE BOOL "" FORCE)
set(IREE_HAL_DRIVER_METAL ON CACHE BOOL "" FORCE)
set(IREE_HAL_EXECUTABLE_LOADER_DEFAULTS OFF CACHE BOOL "" FORCE)
set(IREE_HAL_EXECUTABLE_PLUGIN_DEFAULTS OFF CACHE BOOL "" FORCE)
set(IREE_ENABLE_CPUINFO OFF CACHE BOOL "" FORCE)
set(IREE_ENABLE_RUNTIME_TRACING OFF CACHE BOOL "" FORCE)
set(IREE_ERROR_ON_MISSING_SUBMODULES OFF CACHE BOOL "" FORCE)

add_subdirectory("${XMOJO_IREE_ROOT}" iree EXCLUDE_FROM_ALL)

add_library(xmojo_gpu STATIC "${XMOJO_ROOT}/src/gpu/Runtime.mm")
target_compile_features(xmojo_gpu PUBLIC cxx_std_20)
target_include_directories(xmojo_gpu PUBLIC "${XMOJO_ROOT}/include")
target_link_libraries(
  xmojo_gpu
  PRIVATE
    iree::async::util::proactor_pool
    iree::base::internal::flatcc::building
    iree::hal::drivers::metal::metal
    iree::schemas::metal_executable_def_c_fbs
)

add_executable(xmojo_gpu_test iree_hal_test.cpp)
target_compile_features(xmojo_gpu_test PRIVATE cxx_std_20)
target_link_libraries(xmojo_gpu_test PRIVATE xmojo_gpu)

add_test(NAME xmojo_gpu_test COMMAND xmojo_gpu_test)
