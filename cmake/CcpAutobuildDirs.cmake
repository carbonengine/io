# Wrappers for add_library, add_executable, etc to automatically place them in the
# expected autobuild folders in Perforce.
# WARNING: we are relying on undocumented CMake behaviour here. The gory details
# were taken from https://www.youtube.com/watch?v=bsXLMQ6WgIk&t=52m38s


include(cmake/CcpTargetConfigurations.cmake)

function(set_target_autobuild_directory target)
    ensure_correct_target_type(${target})
    file(RELATIVE_PATH project_subfolder ${CMAKE_SOURCE_DIR} ${CMAKE_CURRENT_LIST_FILE})
    string(REGEX MATCH "eve|carbon" carbon_or_eve ${project_subfolder})
    if(NOT carbon_or_eve)
        message(STATUS "Not setting autobuild folder for ${target} because this is neither a carbon nor eve project")
        return()
    endif()
    # work-around multi-configuration behaviour (Xcode, VS): adding $<$<CONFIG:DEBUG>:> avoids appending a configuration path
    set(output_directory "${CMAKE_SOURCE_DIR}/${carbon_or_eve}/autobuild/${target}/${CCP_PLATFORM}/${CCP_ARCHITECTURE}/${CCP_TOOLSET}/$<$<CONFIG:DEBUG>:>")
    message(STATUS "Setting ${target} output directory to ${output_directory}")
    set_target_properties(${target}
        PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY ${output_directory}
            LIBRARY_OUTPUT_DIRECTORY ${output_directory}
            RUNTIME_OUTPUT_DIRECTORY ${output_directory}
            PDB_OUTPUT_DIRECTORY ${output_directory}
    )
    set_prefix_and_suffix(${target})
endfunction()
