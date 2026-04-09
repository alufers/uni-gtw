cmake_minimum_required(VERSION 3.16)
project(cosmo_host_test C)

include(FetchContent)
FetchContent_Declare(
    unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.0
)
FetchContent_MakeAvailable(unity)

add_executable(test_cosmo
    test_cosmo.c
    ../cosmo.c
)

target_compile_definitions(test_cosmo PRIVATE HOST_TEST)
target_include_directories(test_cosmo PRIVATE
    ../include
    ${unity_SOURCE_DIR}/src
)
target_link_libraries(test_cosmo unity)

enable_testing()
add_test(NAME cosmo_tests COMMAND test_cosmo)
