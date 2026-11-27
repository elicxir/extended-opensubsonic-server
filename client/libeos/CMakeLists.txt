# libeos: the client library for eos (no UI). Builds on its own, or as a subdirectory
# of the eos repository or of an application:
#
#   add_subdirectory(path/to/eos/client/libeos)
#   target_link_libraries(app PRIVATE eos::client)
cmake_minimum_required(VERSION 3.21)
project(libeos VERSION 0.1.0 LANGUAGES CXX)

option(LIBEOS_BUILD_TESTS "Build the libeos tests" ${PROJECT_IS_TOP_LEVEL})

# 8.11 is the first libcurl with the WebSocket API that /ext/session will use.
find_package(CURL 8.11 REQUIRED)
find_package(nlohmann_json 3 REQUIRED)

add_library(eos_client STATIC
  src/api.cpp
  src/api_ext.cpp
  src/api_rest.cpp
  src/http.cpp
  src/parse.cpp)
add_library(eos::client ALIAS eos_client)
set_target_properties(eos_client PROPERTIES OUTPUT_NAME eos POSITION_INDEPENDENT_CODE ON)
target_compile_features(eos_client PUBLIC cxx_std_20)
target_include_directories(eos_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# Neither libcurl nor nlohmann/json appears in the public headers.
target_link_libraries(eos_client PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
target_compile_definitions(eos_client PRIVATE LIBEOS_VERSION="${PROJECT_VERSION}")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(eos_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(LIBEOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
