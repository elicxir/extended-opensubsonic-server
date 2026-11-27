find_package(GTest REQUIRED)
include(GoogleTest)

# Unit tests: no server needed.
add_executable(libeos_tests test_parse.cpp test_http.cpp)
target_link_libraries(libeos_tests PRIVATE eos::client nlohmann_json::nlohmann_json GTest::gtest_main)
target_include_directories(libeos_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
gtest_discover_tests(libeos_tests DISCOVERY_TIMEOUT 60)

# Live tests: talk to a real eosd. They skip unless EOS_TEST_URL is set; live.sh
# starts a throwaway server with a generated library and sets it.
add_executable(libeos_live_tests test_live.cpp)
target_link_libraries(libeos_live_tests PRIVATE eos::client GTest::gtest_main)

if(TARGET eosd AND TARGET eos-admin)
  add_test(NAME libeos_live
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/live.sh $<TARGET_FILE:eosd> $<TARGET_FILE:eos-admin>
                   $<TARGET_FILE:libeos_live_tests>)
  set_tests_properties(libeos_live PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
endif()
