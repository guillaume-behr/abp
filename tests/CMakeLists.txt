add_executable(abp_tests
    main_test.cpp
    test_stringutil.cpp
    test_json.cpp
    test_sha256.cpp
    test_manifest.cpp
)

target_link_libraries(abp_tests PRIVATE abp_core)
target_include_directories(abp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
abp_set_warnings(abp_tests)

add_test(NAME abp_tests COMMAND abp_tests)
