enable_testing()
project(queue_test)

add_executable(${PROJECT_NAME} test.cpp)
add_test(NAME ${PROJECT_NAME} COMMAND ${PROJECT_NAME})

target_link_libraries(
    ${PROJECT_NAME}
    async
)
