cmake_minimum_required(VERSION 3.16)
project(io VERSION 1.0.0)

if(NOT DEFINED ENV{CCP_EVE_PERFORCE_BRANCH_PATH})
    message(FATAL_ERROR "Missing required environment variable CCP_EVE_PERFORCE_BRANCH_PATH")
elseif(NOT IS_ABSOLUTE $ENV{CCP_EVE_PERFORCE_BRANCH_PATH})
    message(FATAL_ERROR "CCP_EVE_PERFORCE_BRANCH_PATH needs to be an absolute path")
elseif(NOT EXISTS $ENV{CCP_EVE_PERFORCE_BRANCH_PATH})
    message(FATAL_ERROR "CCP_EVE_PERFORCE_BRANCH_PATH is pointing at a non-existing location $ENV{CCP_EVE_PERFORCE_BRANCH_PATH}")
elseif(NOT IS_DIRECTORY $ENV{CCP_EVE_PERFORCE_BRANCH_PATH})
    message(FATAL_ERROR "CCP_EVE_PERFORCE_BRANCH_PATH needs to be a directory")
else()
    message(STATUS "Using CCP_EVE_PERFORCE_BRANCH_PATH located at $ENV{CCP_EVE_PERFORCE_BRANCH_PATH}")
endif()

file(TO_CMAKE_PATH "$ENV{CCP_EVE_PERFORCE_BRANCH_PATH}" BRANCH_ROOT_DIR)
list(INSERT CMAKE_MODULE_PATH 0 ${BRANCH_ROOT_DIR}/cmake)

include(CcpGlobalSettings)
include(CcpPackageConfigHelpers)

find_package(libuv CONFIG REQUIRED)
find_package(Python CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)
find_package(CcpLog CONFIG REQUIRED)
find_package(CcpCore CONFIG REQUIRED)
find_package(Scheduler CONFIG REQUIRED)

# --- _socket ---
ccp_add_library(_socket SHARED
        src/carbonio.cpp
        src/socketmodule.cpp
)
target_precompile_headers(_socket PRIVATE
        <memory>
        <string>
        <string_view>

        <CcpLog.h>
        <CcpScopeGuard.h>
)
target_link_libraries(_socket
        PRIVATE
        libuv::uv_a
        ccp::log
        CcpCore
        Python
)
if(WIN32)
    target_link_libraries(_socket
            PRIVATE
            Rpcrt4
    )
endif()
set_target_properties(_socket
        PROPERTIES
        DEBUG_POSTFIX ""
        INTERNAL_POSTFIX ""
)

# ---

get_target_property(SCHEDULER_INCLUDE_DIR Scheduler INTERFACE_INCLUDE_DIRECTORIES)

target_include_directories(_socket
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${SCHEDULER_INCLUDE_DIR}
)

# The following variable is defined only on DLL systems
if(CMAKE_IMPORT_LIBRARY_SUFFIX)
    add_custom_command(
            TARGET _socket POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:_socket> $<TARGET_FILE_DIR:_socket>
            COMMAND_EXPAND_LISTS
    )
endif()

get_target_property(_SOURCES _socket SOURCES)
source_group("_socket" FILES
        ${_SOURCES}
        src/carbonio.h
        include/socketmodule.h
)

# The following variable is defined only on DLL systems
if (CMAKE_IMPORT_LIBRARY_SUFFIX)
    add_custom_command(
            TARGET _socket POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_RUNTIME_DLLS:_socket> $<TARGET_FILE_DIR:_socket>
            COMMAND_EXPAND_LISTS
    )
endif ()



# Define an alias target so we can export a target with the expected project name
add_library(carbonio ALIAS _socket)

# --- _ssl ---
ccp_add_library(_ssl SHARED
        src/_ssl.h
        src/_ssl.c
        src/c_channel.h
        src/c_channel.cpp
)
get_target_property(_SOURCES _ssl SOURCES)
source_group("_ssl" FILES
        ${_SOURCES}
        src/_ssl_data.h
        src/_ssl/debughelpers.c
        src/clinic/_ssl.c.h
)
target_link_libraries(_ssl
        PRIVATE
        Python
        PUBLIC
        OpenSSL::Crypto
        OpenSSL::SSL
)
set_target_properties(_ssl
        PROPERTIES
        DEBUG_POSTFIX ""
        INTERNAL_POSTFIX ""
)
target_include_directories(_ssl
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${SCHEDULER_INCLUDE_DIR}
)

# --- _select ---
ccp_add_library(_select SHARED
        src/selectmodule.c
)
set_target_properties(_select
        PROPERTIES
        DEBUG_POSTFIX ""
        INTERNAL_POSTFIX ""
        OUTPUT_NAME select
)
get_target_property(_SOURCES _select SOURCES)
source_group("_select" FILES
        ${_SOURCES}
        src/clinic/selectmodule.c.h
)
target_link_libraries(_select
        PRIVATE
        Python
)
if(WIN32)
    target_link_libraries(_select PUBLIC ws2_32.lib)
endif()

# Provide an install target if this is the top level project only
if(CMAKE_PROJECT_NAME STREQUAL io)
    option(BUILD_TESTING "Build tests. Enabled by default." ON)
    include(CTest)
    if(BUILD_TESTING)
        # include the C++ test suite
        add_subdirectory(tests)
        # discover python tests ... this may be a little bit brittle since we're working around the missing `_blueexposuretest`
        # binary. As in, test discovery happens during the configure stage at which the binary does not exist yet.
        if(WIN32)
            set(ENV{PYTHONPATH} ${BRANCH_ROOT_DIR}/carbon/common/stdlib\;${Python_ROOT}/${CCP_VENDOR_BIN_PATH}\;${CMAKE_CURRENT_SOURCE_DIR}/python\;${Scheduler_ROOT}/${CCP_VENDOR_BIN_PATH}\;${Greenlet_ROOT}/${CCP_VENDOR_BIN_PATH}\;${Greenlet_ROOT}/python)
        elseif(APPLE)
            set(ENV{PYTHONPATH} ${BRANCH_ROOT_DIR}/carbon/common/stdlib:${Python_ROOT}/${CCP_VENDOR_BIN_PATH}:${CMAKE_CURRENT_SOURCE_DIR}/python:${Scheduler_ROOT}/${CCP_VENDOR_LIB_PATH}:${Greenlet_ROOT}/${CCP_VENDOR_BIN_PATH}\:${Greenlet_ROOT}/python)
        endif()
        # Discovery works with the standard python interpreter because it isn't running anything, it just needs to find all
        # the defined test cases. Therefore, just use that one, even though it cannot run the discovered test cases.
        execute_process(COMMAND ${PYTHON_EXECUTABLE} discover.py
                OUTPUT_VARIABLE PYTHON_TESTS_STR
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE PYTHON_TEST_DISCOVERY_RETVAL
                ERROR_VARIABLE PYTHON_TEST_DISCOVERY_ERRORS
                ECHO_ERROR_VARIABLE
                ECHO_OUTPUT_VARIABLE
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/python
        )
        if(${PYTHON_TEST_DISCOVERY_RETVAL})
            message(FATAL_ERROR "Error encountered during test discovery:\n=== stderr: ===\n${PYTHON_TEST_DISCOVERY_ERRORS}\n=== stdout: ===\n${PYTHON_TESTS_STR}")
        endif()
        separate_arguments(PYTHON_TESTS UNIX_COMMAND ${PYTHON_TESTS_STR})
        foreach(PYTHON_TEST ${PYTHON_TESTS})
            message(STATUS "Found test ${PYTHON_TEST}")
            add_test(NAME ${PYTHON_TEST}
                    COMMAND ${PYTHON_EXECUTABLE} -m carboniotests -v ${PYTHON_TEST}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/python
            )
            if(WIN32)
                set_tests_properties(${PYTHON_TEST} PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:_socket>\;${BRANCH_ROOT_DIR}/carbon/common/stdlib\;${Python_ROOT}/${CCP_VENDOR_BIN_PATH}\;${Scheduler_ROOT}/python\;${Scheduler_ROOT}/${CCP_VENDOR_BIN_PATH}\;${Greenlet_ROOT}/${CCP_VENDOR_BIN_PATH}\;${Greenlet_ROOT}/python\;${CMAKE_CURRENT_SOURCE_DIR}/python;BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>")
            elseif (APPLE)
                set_tests_properties(${PYTHON_TEST} PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:_socket>:${BRANCH_ROOT_DIR}/carbon/common/stdlib:${Python_ROOT}/${CCP_VENDOR_BIN_PATH}:${Scheduler_ROOT}/${CCP_VENDOR_LIB_PATH}:${Scheduler_ROOT}/python:${Greenlet_ROOT}/${CCP_VENDOR_BIN_PATH}:${Greenlet_ROOT}/python:${CMAKE_CURRENT_SOURCE_DIR}/python;BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>")
            else()
                message(FATAL_ERROR "Unsupported platform")
            endif ()
        endforeach()
    endif()

    # Install rule to ensure that our runtime and linker files are in the expected, platform-specific folders
    install(
            TARGETS _socket _ssl _select
            EXPORT ioTarget
            CONFIGURATIONS Release
            RUNTIME DESTINATION bin/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/
            LIBRARY DESTINATION lib/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/
    )

    # Install rule for available public headers
    install(DIRECTORY include DESTINATION . FILES_MATCHING PATTERN "*.h")

    configure_ccp_vendor_config_file(
            TARGET carbonio
            DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/CarbonIOConfig.cmake"
            DEPENDENCIES "Python;OpenSSL;CcpCore;CcpLog;libuv"
            CONFIGURATIONS Release
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/CarbonIOConfig.cmake DESTINATION .)
endif()
