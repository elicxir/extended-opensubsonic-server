find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(eos_tests
  test_strings.cpp
  test_normalize.cpp
  test_ids_pid.cpp
  test_config.cpp
  test_crypto.cpp
  test_log.cpp
  test_db.cpp
  test_users.cpp
  test_scanner.cpp
  test_api_units.cpp
  test_api.cpp
  test_m3.cpp
  test_ext.cpp
  test_sync.cpp
  fixtures.cpp)
target_link_libraries(eos_tests PRIVATE eoslib GTest::gtest_main)
target_include_directories(eos_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(eos_tests DISCOVERY_TIMEOUT 60)
