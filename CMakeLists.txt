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
set(libuv_DIR ${BRANCH_ROOT_DIR}/vendor/github.com/libuv/libuv/v1.47.0)

include(CcpGlobalSettings)
include(CcpPackageConfigHelpers)

find_package(libuv CONFIG REQUIRED)
find_package(Python CONFIG REQUIRED)
find_package(OpenSSL CONFIG REQUIRED)

add_library(iobase STATIC
    src/carbonio.cpp
)
target_link_libraries(iobase
    PUBLIC
        Python
        libuv::uv_a
)
target_include_directories(iobase
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(iobase PUBLIC BLUEIMPORT=)

# --- _socket ---
ccp_add_library(_socket SHARED
    src/socketmodule.cpp
)
target_link_libraries(_socket
    PRIVATE
        iobase
)
set_target_properties(_socket
        PROPERTIES
        DEBUG_POSTFIX ""
)

get_target_property(_SOURCES _socket SOURCES)
source_group("_socket" FILES
    ${_SOURCES}
    src/socketmodule.h
)

# --- _ssl ---
ccp_add_library(_ssl SHARED
    src/_ssl.c
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
        iobase
    PUBLIC
        OpenSSL::Crypto
        OpenSSL::SSL
)

# --- _select ---
ccp_add_library(_select SHARED
    src/selectmodule.c
)
get_target_property(_SOURCES _select SOURCES)
source_group("_select" FILES
    ${_SOURCES}
    src/clinic/selectmodule.c.h
)
target_link_libraries(_select
    PRIVATE
        iobase
)
if(WIN32)
    target_link_libraries(_select PUBLIC ws2_32.lib)
endif()

# Provide an install target if this is the top level project only
if(CMAKE_PROJECT_NAME STREQUAL io)
    option(BUILD_TESTING "Build tests. Enabled by default." ON)
    include(CTest)
    if(BUILD_TESTING)
        list(APPEND PYTHON_TESTS test_socket.py)
        list(APPEND PYTHON_TESTS test_ssl.py)
        list(APPEND PYTHON_TESTS test_select.py)
        foreach(PYTHON_TEST ${PYTHON_TESTS})
            add_test(NAME ${PYTHON_TEST}
                    COMMAND ${PYTHON_EXECUTABLE} ${PYTHON_TEST}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests
            )
            if(WIN32)
                set_tests_properties(${PYTHON_TEST} PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:_socket>\;${BRANCH_ROOT_DIR}/carbon/common/stdlib\;${BRANCH_ROOT_DIR}/vendor/python/3.8.1+ccp-stackless/${CCP_VENDOR_BIN_PATH};BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>")
            elseif (APPLE)
                set_tests_properties(${PYTHON_TEST} PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:_socket>:${BRANCH_ROOT_DIR}/carbon/common/stdlib:${BRANCH_ROOT_DIR}/vendor/python/3.8.1+ccp-stackless/${CCP_VENDOR_BIN_PATH};BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>")
            else()
                message(FATAL_ERROR "Unsupported platform")
            endif ()
        endforeach()
    endif()

    # Install rule to ensure that our runtime and linker files are in the expected, platform-specific folders
#    install(
#            TARGETS io
#            EXPORT ioTarget
#            CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES}
#            LIBRARY DESTINATION lib/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/
#            ARCHIVE DESTINATION lib/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/
#            RUNTIME DESTINATION bin/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/
#    )
    # Install rule for available public headers
#    install(FILES ${PUBLIC_HEADER_FILES} DESTINATION include)

#    configure_ccp_vendor_config_file(
#            TARGET io
#            DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/ioConfig.cmake"
#            DEPENDENCIES ""
#    )
#    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ioConfig.cmake DESTINATION .)

    # Install python package
#    install(DIRECTORY python/ DESTINATION io
#            FILES_MATCHING
#            PATTERN "*.py"
#            PATTERN "package.yaml"
#            )
endif()
