cmake_minimum_required(VERSION 3.20)
project(xmojo_dependencies LANGUAGES C CXX)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install ON CACHE BOOL "" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ENABLE_CURVE OFF CACHE BOOL "" FORCE)
set(ENABLE_DRAFTS OFF CACHE BOOL "" FORCE)
set(WITH_LIBSODIUM OFF CACHE BOOL "" FORCE)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XEUS_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(XEUS_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(XEUS_STATIC_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(XEUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XEUS_ZMQ_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(XEUS_ZMQ_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(XEUS_ZMQ_STATIC_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(XEUS_ZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(XEUS_ZMQ_BUILD_WITHOUT_LIBSODIUM ON CACHE BOOL "" FORCE)

set(BORINGSSL_INCLUDE_DIR "" CACHE PATH "Modular BoringSSL include directory")
if(NOT EXISTS "${BORINGSSL_INCLUDE_DIR}/openssl/hmac.h")
  message(FATAL_ERROR "BORINGSSL_INCLUDE_DIR must name Modular's BoringSSL include directory")
endif()
add_library(OpenSSL::Crypto INTERFACE IMPORTED GLOBAL)
set_target_properties(
  OpenSSL::Crypto PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${BORINGSSL_INCLUDE_DIR}"
)

get_filename_component(SIBLING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

foreach(dependency nlohmann-json libzmq cppzmq xeus xeus-zmq)
  if(NOT EXISTS "${SIBLING_ROOT}/${dependency}/CMakeLists.txt")
    message(FATAL_ERROR "Missing sibling checkout: ${SIBLING_ROOT}/${dependency}")
  endif()
endforeach()

add_subdirectory("${SIBLING_ROOT}/nlohmann-json" nlohmann-json)
add_subdirectory("${SIBLING_ROOT}/libzmq" libzmq)
add_subdirectory("${SIBLING_ROOT}/cppzmq" cppzmq)
add_subdirectory("${SIBLING_ROOT}/xeus" xeus)
add_subdirectory("${SIBLING_ROOT}/xeus-zmq" xeus-zmq)

# xeus-zmq's OpenSSL implementation offers algorithms that BoringSSL does not.
# The Jupyter protocol only needs hmac-sha256 (or no authentication), so build
# the transport with our deliberately narrow BoringSSL implementation instead.
get_target_property(XEUS_ZMQ_SOURCES xeus-zmq-static SOURCES)
list(REMOVE_ITEM XEUS_ZMQ_SOURCES
  "${SIBLING_ROOT}/xeus-zmq/src/common/xauthentication.cpp"
)
set_property(TARGET xeus-zmq-static PROPERTY SOURCES "${XEUS_ZMQ_SOURCES}")
target_sources(xeus-zmq-static PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/transport/BoringSSLAuthentication.cpp"
)
target_include_directories(xeus-zmq-static PRIVATE
  "${SIBLING_ROOT}/xeus-zmq/src/common"
)
