ccp_add_library(sockettesthelper SHARED
        sockettesthelper.cpp
)

target_link_libraries(sockettesthelper
        PRIVATE
        Python
)

set_target_properties(sockettesthelper
        PROPERTIES
        DEBUG_POSTFIX ""
        INTERNAL_POSTFIX ""
        TRINITYDEV_POSTFIX ""
)

target_include_directories(sockettesthelper
        PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../../include
)
