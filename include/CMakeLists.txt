file (GLOB_RECURSE HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/async/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_resource/*.hpp
)

add_library (
    ${PROJECT_NAME}
    INTERFACE
)

target_sources (
    ${PROJECT_NAME}
    INTERFACE
    ${HEADERS}
)

target_link_libraries(
    ${PROJECT_NAME}
    INTERFACE
    Boost::system
    Boost::context
    Boost::coroutine
)

target_include_directories (
    ${PROJECT_NAME}
    INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
