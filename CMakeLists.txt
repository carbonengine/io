# Copyright © 2026 CCP ehf.

cmake_minimum_required(VERSION 3.30)

project(io VERSION 1.0.0)

include(cmake/CcpTargetConfigurations.cmake)
include(cmake/CcpDocsGenerator.cmake)
include(cmake/CcpPackageConfigHelpers.cmake)
include(cmake/CcpBuildConfigurations.cmake)

find_package(libuv CONFIG REQUIRED)
find_package(Python3 COMPONENTS Development Interpreter REQUIRED)

set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL COMPONENTS Crypto SSL REQUIRED)
find_package(carbon-core CONFIG REQUIRED)
find_package(carbon-scheduler CONFIG REQUIRED)


# --- _socket ---
ccp_add_library(_socket SHARED
        src/carbonio.cpp
        src/socketmodule.cpp
)
target_precompile_headers(_socket PRIVATE
        <memory>
        <string>
        <string_view>
        <CCPLog.h>
        <CcpMacros.h>
        <CcpScopeGuard.h>
)
target_link_libraries(_socket
        PRIVATE
        $<IF:$<TARGET_EXISTS:libuv::uv_a>,libuv::uv_a,libuv::uv>
        CcpCore
        Python3::Python
)
if(WIN32)
    target_link_libraries(_socket
            PRIVATE
            Rpcrt4
    )
endif()
set_target_properties(_socket
        PROPERTIES
        OUTPUT_NAME _carbonsocket
)

get_target_property(SCHEDULER_INCLUDE_DIR Scheduler INTERFACE_INCLUDE_DIRECTORIES)

target_include_directories(_socket
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
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
        Python3::Python
        libuv::uv
        CcpCore
        PUBLIC
        OpenSSL::Crypto
        OpenSSL::SSL
)
target_precompile_headers(_ssl PRIVATE
        <CcpMacros.h>
)
set_target_properties(_ssl
        PROPERTIES
        OUTPUT_NAME _carbonssl
)
target_include_directories(_ssl
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
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
        OUTPUT_NAME carbonselect
)
get_target_property(_SOURCES _select SOURCES)
source_group("_select" FILES
        ${_SOURCES}
        src/clinic/selectmodule.c.h
)
target_link_libraries(_select
        PRIVATE
        CcpCore
        Python3::Python
)
target_precompile_headers(_select PRIVATE
        <CcpMacros.h>
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

        if(WIN32)
            set(ENV{PYTHONPATH} ${CMAKE_CURRENT_SOURCE_DIR}/python)
        elseif(APPLE)
            set(ENV{PYTHONPATH} ${CMAKE_CURRENT_SOURCE_DIR}/python)
        endif()
        # Discovery works with the standard python interpreter because it isn't running anything, it just needs to find all
        # the defined test cases. Therefore, just use that one, even though it cannot run the discovered test cases.
        execute_process(COMMAND ${Python3_EXECUTABLE} discover.py
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
                    COMMAND ${Python3_EXECUTABLE} -m carboniotests -v ${PYTHON_TEST}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/python
            )
            if(WIN32)
                set_tests_properties(
                        ${PYTHON_TEST}
                        PROPERTIES
                        ENVIRONMENT
                        "PYTHONPATH=$<TARGET_FILE_DIR:_socket>\;$<TARGET_FILE_DIR:sockettesthelper>\;${CMAKE_CURRENT_SOURCE_DIR}/python\;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin\;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/python\;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib;BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>"
                )
            elseif (APPLE)
                set_tests_properties(
                        ${PYTHON_TEST}
                        PROPERTIES
                        ENVIRONMENT
                        "PYTHONPATH=$<TARGET_FILE_DIR:_socket>:$<TARGET_FILE_DIR:sockettesthelper>:${CMAKE_CURRENT_SOURCE_DIR}/python:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/python:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib;BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>"
                )
            else()
                message(FATAL_ERROR "Unsupported platform")
            endif ()
        endforeach()
    endif()

    # Documentation generation is constrained to release builds
    if(${CMAKE_BUILD_TYPE} MATCHES Release)
        set(BUILD_DOCUMENTATION_DEFAULT_FLAG OFF)
        if (DEFINED ENV{TEAMCITY_VERSION})
            set(BUILD_DOCUMENTATION_DEFAULT_FLAG ON)
        endif()
        option(BUILD_DOCUMENTATION "Build documentation" ${BUILD_DOCUMENTATION_DEFAULT_FLAG})
        message(STATUS "Document generation settings: default=${BUILD_DOCUMENTATION_DEFAULT_FLAG}, option BUILD_DOCUMENTATION=${BUILD_DOCUMENTATION}")
        if(BUILD_DOCUMENTATION)
            # Evaluate config file for Doxygen to input project values
            set(DOXYFILE_IN ${CMAKE_CURRENT_SOURCE_DIR}/doc/Doxyfile.in)
            set(DOXYFILE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
            set(DOXYGEN_INDEX_FILE ${CMAKE_CURRENT_BINARY_DIR}/xml/index.xml)
            configure_file(${DOXYFILE_IN} ${DOXYFILE_OUT} @ONLY)

            include(cmake/CcpDocsGenerator.cmake)

            # Run sphinx
            if(WIN32)
                set(PYTHONPATH_ENV_SPHINX "${CMAKE_SOURCE_DIR}/python/\;$<TARGET_FILE_DIR:_socket>\;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin\;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/python\;${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
            elseif(APPLE)
                set(PYTHONPATH_ENV_SPHINX "${CMAKE_SOURCE_DIR}/python/:$<TARGET_FILE_DIR:_socket>:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/python:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
            endif()

            set(SPHINX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/doc/source)
            set(SPHINX_BUILD ${CMAKE_CURRENT_BINARY_DIR}/doc/build)

            create_carbon_docs_sphinx_target(
                    PYTHON_EXE ${Python3_EXECUTABLE}
                    VENV_NAME docs_generation_venv
                    PYTHONPATH_ENV ${PYTHONPATH_ENV_SPHINX}
                    SPHINX_SOURCE ${SPHINX_SOURCE}
                    SPHINX_BUILD ${SPHINX_BUILD}
                    DOXYGEN_SRC_FILES ${SRC_FILES}
                    SPHINX_TARGET_NAME Sphinx
                    DOXYGEN_TARGET_NAME Doxygen
                    INSTALL_DESTINATION documentation/carbon-io
            )

            # Ensure that _socket is built before Spinx target
            add_dependencies(Sphinx _socket)
        endif()
    endif()

    option (INSTALL_TO_MONOLITH "Install this package to a perforce branch vendor folder - provided for backwards compatability" OFF)
    if(INSTALL_TO_MONOLITH)
        # Install rule to ensure that our runtime and linker files are in the expected, platform-specific folders
        install(
                TARGETS _socket _ssl _select
                EXPORT ioTarget
                CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES}
                RUNTIME DESTINATION bin/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/
                LIBRARY DESTINATION lib/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/
        )

        # Install rule for available public headers
        install(DIRECTORY include DESTINATION . FILES_MATCHING PATTERN "*.h")

        configure_ccp_vendor_config_file(
                TARGET carbonio
                DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/CarbonIOConfig.cmake"
                DEPENDENCIES "Python;OpenSSL;CcpCore;libuv"
                CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES}
        )
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/CarbonIOConfig.cmake DESTINATION .)

    else() # VCPKG install mode

        install(TARGETS _socket _ssl _select
                EXPORT ioTarget
                CONFIGURATIONS ${CMAKE_CONFIGURATION_TYPES}
                RUNTIME DESTINATION bin/
                LIBRARY DESTINATION lib/
        )

        install(EXPORT ioTarget
            DESTINATION share/carbon-io
            FILE carbon-ioConfig.cmake
        )

        install(DIRECTORY include DESTINATION . FILES_MATCHING PATTERN "*.h")
    endif()
endif()
