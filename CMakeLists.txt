cmake_minimum_required(VERSION 3.25)
project(eos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(EOS_BUILD_TESTS "Build the test suite" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3>=3.38)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
find_package(OpenSSL 3.2 REQUIRED)
find_package(TagLib 2 CONFIG REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(tomlplusplus 3 REQUIRED)
find_package(Threads REQUIRED)
# Drogon: link the shared libraries directly. Its CMake package config insists on the
# development files of every optional backend (PostgreSQL, MySQL, Redis, ...).
find_path(DROGON_INCLUDE_DIR drogon/drogon.h REQUIRED)
find_library(DROGON_LIBRARY drogon REQUIRED)
find_library(TRANTOR_LIBRARY trantor REQUIRED)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
add_library(eos_drogon INTERFACE)
target_include_directories(eos_drogon INTERFACE ${DROGON_INCLUDE_DIR})
target_link_libraries(eos_drogon INTERFACE ${DROGON_LIBRARY} ${TRANTOR_LIBRARY} PkgConfig::JSONCPP)

# Embed server/db/migrations/*.sql into the binary.
file(GLOB EOS_MIGRATION_FILES CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/server/db/migrations/*.sql)
list(SORT EOS_MIGRATION_FILES)
string(REPLACE ";" "|" EOS_MIGRATION_FILES_ARG "${EOS_MIGRATION_FILES}")
set(EOS_MIGRATIONS_CPP ${CMAKE_BINARY_DIR}/generated/migrations_data.cpp)
add_custom_command(
  OUTPUT ${EOS_MIGRATIONS_CPP}
  COMMAND ${CMAKE_COMMAND}
          -DOUT=${EOS_MIGRATIONS_CPP}
          "-DFILES=${EOS_MIGRATION_FILES_ARG}"
          -P ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
  DEPENDS ${EOS_MIGRATION_FILES} ${CMAKE_SOURCE_DIR}/cmake/embed_migrations.cmake
  COMMENT "Embedding SQL migrations"
  VERBATIM)

add_library(eoslib STATIC
  server/core/config.cpp
  server/core/crypto.cpp
  server/core/ids.cpp
  server/core/log.cpp
  server/core/normalize.cpp
  server/core/strings.cpp
  server/db/database.cpp
  server/db/migrate.cpp
  server/db/write_queue.cpp
  server/auth/users.cpp
  server/scanner/pid.cpp
  server/scanner/tag_reader.cpp
  server/scanner/track_info.cpp
  server/scanner/scanner.cpp
  server/scanner/tag_report.cpp
  server/scanner/derived.cpp
  server/search/index.cpp
  server/suggest/rules.cpp
  server/media/artwork.cpp
  server/media/transcode.cpp
  server/api/http.cpp
  server/api/net.cpp
  server/api/node.cpp
  server/api/rate_limit.cpp
  server/api/subsonic/auth.cpp
  server/api/subsonic/builders.cpp
  server/api/subsonic/context.cpp
  server/api/subsonic/router.cpp
  server/api/subsonic/h_system.cpp
  server/api/subsonic/h_browse.cpp
  server/api/subsonic/h_lists.cpp
  server/api/subsonic/h_search.cpp
  server/api/subsonic/h_annotate.cpp
  server/api/subsonic/h_playlists.cpp
  server/api/subsonic/h_queue.cpp
  server/api/subsonic/h_media.cpp
  server/api/ext/router.cpp
  server/api/ext/model.cpp
  server/api/ext/suggestions.cpp
  server/api/ext/h_read.cpp
  server/api/ext/h_write.cpp
  server/api/ext/h_suggest.cpp
  server/api/ext/h_account.cpp
  server/api/ext/h_transfer.cpp
  server/http/server.cpp
  server/http/session_socket.cpp
  server/sync/hub.cpp
  ${EOS_MIGRATIONS_CPP})
target_include_directories(eoslib PUBLIC ${CMAKE_SOURCE_DIR}/server)
target_compile_definitions(eoslib PUBLIC EOS_VERSION="${PROJECT_VERSION}")
target_link_libraries(eoslib PUBLIC
  PkgConfig::SQLITE3 PkgConfig::UTF8PROC
  OpenSSL::Crypto TagLib::tag
  nlohmann_json::nlohmann_json tomlplusplus::tomlplusplus
  eos_drogon Threads::Threads)
target_compile_options(eoslib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(eosd server/main.cpp)
target_link_libraries(eosd PRIVATE eoslib)
target_compile_options(eosd PRIVATE -Wall -Wextra -Wpedantic)

add_executable(eos-admin tools/eos-admin/main.cpp)
target_link_libraries(eos-admin PRIVATE eoslib)
target_compile_options(eos-admin PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS eosd eos-admin RUNTIME DESTINATION bin)

if(EOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
