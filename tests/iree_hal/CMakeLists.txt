cmake_minimum_required(VERSION 3.25)

project(xmojo_iree_hal_probe LANGUAGES C CXX OBJC)

enable_testing()

get_filename_component(XMOJO_DEFAULT_IREE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
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

add_executable(xmojo_iree_hal_probe iree_hal_test.cpp metal_device_probe.m)
target_compile_features(xmojo_iree_hal_probe PRIVATE cxx_std_20)
target_link_libraries(
  xmojo_iree_hal_probe
  PRIVATE
    iree::async::util::proactor_pool
    iree::base::internal::flatcc::building
    iree::hal::drivers::metal::metal
    iree::schemas::metal_executable_def_c_fbs
)

add_test(NAME xmojo_iree_hal_probe COMMAND xmojo_iree_hal_probe)
