cmake_minimum_required(VERSION 3.31)

project(io VERSION 1.0.0)

include(cmake/CcpGlobalSettings.cmake)
include(cmake/CcpVendorUtilities.cmake)
include(cmake/CcpTargetConfigurations.cmake)

find_package(libuv CONFIG REQUIRED)
find_package(Python3 COMPONENTS Development REQUIRED)

set(OPENSSL_USE_STATIC_LIBS ON)
find_package(OpenSSL REQUIRED)
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

        <CcpLog.h>
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
        DEBUG_POSTFIX ""
        INTERNAL_POSTFIX ""
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
        PUBLIC
        OpenSSL::Crypto
        OpenSSL::SSL
)
set_target_properties(_ssl
        PROPERTIES
        DEBUG_POSTFIX ""
        INTERNAL_POSTFIX ""
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
        DEBUG_POSTFIX ""
        INTERNAL_POSTFIX ""
        OUTPUT_NAME carbonselect
)
get_target_property(_SOURCES _select SOURCES)
source_group("_select" FILES
        ${_SOURCES}
        src/clinic/selectmodule.c.h
)
target_link_libraries(_select
        PRIVATE
        Python3::Python
)
if(WIN32)
    target_link_libraries(_select PUBLIC ws2_32.lib)
endif()

# Provide an install target if this is the top level project only
if(CMAKE_PROJECT_NAME STREQUAL io)
    option(BUILD_TESTING "Build tests. Enabled by default." ON)
    include(CTest)

    get_target_property(SCHEDULER_IMPORTED_LOCATION Scheduler IMPORTED_LOCATION)
    get_filename_component(SCHEDULER_IMPORTED_LOCATION_DIR ${SCHEDULER_IMPORTED_LOCATION} DIRECTORY)

    if (WIN32)
        find_program(BUILT_PYTHON_EXECUTABLE python REQUIRED NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH)
    elseif(APPLE)
        find_program(BUILT_PYTHON_EXECUTABLE python3 REQUIRED NO_SYSTEM_ENVIRONMENT_PATH NO_CMAKE_SYSTEM_PATH)
    endif()

    get_filename_component(PYTHON_TOOL_ROOT_DIR ${BUILT_PYTHON_EXECUTABLE} DIRECTORY)

    if(BUILD_TESTING)

        # include the C++ test suite
        add_subdirectory(tests)
        # discover python tests ... this may be a little bit brittle since we're working around the missing `_blueexposuretest`
        # binary. As in, test discovery happens during the configure stage at which the binary does not exist yet.


        if(WIN32)
            set(ENV{PYTHONPATH} ${PYTHON_TOOL_ROOT_DIR}/Lib\;${PYTHON_TOOL_ROOT_DIR}\;${CMAKE_CURRENT_SOURCE_DIR}/python\;${SCHEDULER_IMPORTED_LOCATION_DIR}\;${SCHEDULER_IMPORTED_LOCATION_DIR}/python)
        elseif(APPLE)
            set(ENV{PYTHONPATH} ${PYTHON_TOOL_ROOT_DIR}/Lib:${PYTHON_TOOL_ROOT_DIR}:${CMAKE_CURRENT_SOURCE_DIR}/python:${SCHEDULER_IMPORTED_LOCATION_DIR}:${SCHEDULER_IMPORTED_LOCATION_DIR}/python)
        endif()

        message(STATUS "the python path is $ENV{PYTHONPATH}")
        # Discovery works with the standard python interpreter because it isn't running anything, it just needs to find all
        # the defined test cases. Therefore, just use that one, even though it cannot run the discovered test cases.
        execute_process(COMMAND ${BUILT_PYTHON_EXECUTABLE} discover.py
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
                    COMMAND ${BUILT_PYTHON_EXECUTABLE} -m carboniotests -v ${PYTHON_TEST}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/python
            )
            if(WIN32)
                set_tests_properties(${PYTHON_TEST} PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:_socket>\;$<TARGET_FILE_DIR:sockettesthelper>\;${PYTHON_TOOL_ROOT_DIR}/Lib\;${PYTHON_TOOL_ROOT_DIR}\;${CMAKE_CURRENT_SOURCE_DIR}/python\;${SCHEDULER_IMPORTED_LOCATION_DIR}\;${SCHEDULER_IMPORTED_LOCATION_DIR}/python;BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>")
            elseif (APPLE)
                set_tests_properties(${PYTHON_TEST} PROPERTIES ENVIRONMENT "PYTHONPATH=$<TARGET_FILE_DIR:_socket>:$<TARGET_FILE_DIR:sockettesthelper>:${PYTHON_TOOL_ROOT_DIR}/Lib:${PYTHON_TOOL_ROOT_DIR}:${CMAKE_CURRENT_SOURCE_DIR}/python:${SCHEDULER_IMPORTED_LOCATION_DIR}:${SCHEDULER_IMPORTED_LOCATION_DIR}/python;BUILDFLAVOR=$<LOWER_CASE:$<CONFIG>>")
            else()
                message(FATAL_ERROR "Unsupported platform")
            endif ()
        endforeach()
    endif()

    set(BUILD_DOCUMENTATION_DEFAULT_FLAG ON)
    if(BUILD_DOCUMENTATION)

        # Evaluate config file for Doxygen to input project values
        set(DOXYFILE_IN ${CMAKE_CURRENT_SOURCE_DIR}/doc/Doxyfile.in)
        set(DOXYFILE_OUT ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile)
        set(DOXYGEN_INDEX_FILE ${CMAKE_CURRENT_BINARY_DIR}/xml/index.xml)
        configure_file(${DOXYFILE_IN} ${DOXYFILE_OUT} @ONLY)

        include(cmake/CcpDocsGenerator.cmake)

        #        # Run sphinx
        if(WIN32)
            set(PYTHONPATH_ENV_SPHINX "${CMAKE_SOURCE_DIR}/python/\;$<TARGET_FILE_DIR:_socket>\;${PYTHON_TOOL_ROOT_DIR}/Lib\;${PYTHON_TOOL_ROOT_DIR}\;${SCHEDULER_IMPORTED_LOCATION_DIR}\;${SCHEDULER_IMPORTED_LOCATION_DIR}/python")
        elseif(APPLE)
            set(PYTHONPATH_ENV_SPHINX "${CMAKE_SOURCE_DIR}/python/:$<TARGET_FILE_DIR:_socket>:${PYTHON_TOOL_ROOT_DIR}:${SCHEDULER_IMPORTED_LOCATION_DIR}:${SCHEDULER_IMPORTED_LOCATION_DIR}/python")
        endif()

        set(SPHINX_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/doc/source)
        set(SPHINX_BUILD ${CMAKE_CURRENT_BINARY_DIR}/doc/build)

        create_carbon_docs_sphinx_target(
                PYTHON_EXE ${BUILT_PYTHON_EXECUTABLE}
                VENV_NAME docs_generation_venv
                PYTHONPATH_ENV ${PYTHONPATH_ENV_SPHINX}
                SPHINX_SOURCE ${SPHINX_SOURCE}
                SPHINX_BUILD ${SPHINX_BUILD}
                DOXYGEN_SRC_FILES ${SRC_FILES}
                SPHINX_TARGET_NAME Sphinx
                DOXYGEN_TARGET_NAME Doxygen
                INSTALL_DESTINATION documentation/carbon-io
        )

        # Ensure that Scheduler is built before Spinx target
        add_dependencies(Sphinx Scheduler)
    endif()

    set(INSTALL_BIN_ONLY OFF CACHE BOOL "Do not Install configuration files meant to be read by projects using this project as a dependency (Cmake Config/ header files etc . . .). Set this to ON when installing this component to the perforce vendor folder")

    # Install rule to ensure that our runtime and linker files are in the expected, platform-specific folders
    if(WIN32)
        install(FILES $<TARGET_FILE_DIR:_socket>/_carbonsocket.pyd DESTINATION bin/)
        install(FILES $<TARGET_FILE_DIR:_ssl>/_carbonssl.pyd DESTINATION bin/)
        install(FILES $<TARGET_FILE_DIR:_select>/_carbonselect.pyd DESTINATION bin/)
    elseif(APPLE)
        install(FILES $<TARGET_FILE_NAME:_socket>  DESTINATION lib/)
        install(FILES $<TARGET_FILE_NAME:_ssl>  DESTINATION lib/)
        install(FILES $<TARGET_FILE_NAME:_select>  DESTINATION lib/)
    endif()
    install(FILES python/io/__init__.py DESTINATION bin/python/io/)

    if(NOT INSTALL_BIN_ONLY)
        install(DIRECTORY include DESTINATION . FILES_MATCHING PATTERN "*.h")
        install(FILES carbon-ioConfig.cmake DESTINATION share/carbon-io/)
    endif()
endif()
