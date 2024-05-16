find_package(GTest REQUIRED)
add_executable(SocketCppUnitTests
        main.cpp
        ../../src/carbonio.cpp
)
target_precompile_headers(SocketCppUnitTests
        PRIVATE
        <Python.h>
        <gtest/gtest.h>
        <CcpLog.h>
        <CcpScopeGuard.h>
)
target_include_directories(SocketCppUnitTests
        PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../src
        ${CMAKE_CURRENT_LIST_DIR}/../../include
)
target_link_libraries(SocketCppUnitTests
        GTest::GTest
        libuv::uv_a
        Python
        ccp::log
        CcpCore
)

if(CMAKE_IMPORT_LIBRARY_SUFFIX)
add_custom_command(TARGET SocketCppUnitTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:SocketCppUnitTests> $<TARGET_FILE_DIR:SocketCppUnitTests>
        COMMAND_EXPAND_LISTS
        VERBATIM
)
endif()

# Test discovery needs to be delayed until we are about to run tests as otherwise the above post-build step won't have
# run. Additionally, we set the working directory for the tests to be the folder in which all built-in Python C
# extensions are located. That way we can avoid copying them around.
gtest_discover_tests(SocketCppUnitTests
        DISCOVERY_MODE PRE_TEST
)
