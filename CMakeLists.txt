cmake_minimum_required(VERSION 3.16)

project (async CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(GCC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-deprecated-declarations -Wno-comments -Wsuggest-override -Werror")
endif(GCC)

if (NOT DEFINED Boost_FOUND)
    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost COMPONENTS system context coroutine REQUIRED)
endif()

add_subdirectory(include)
add_subdirectory(tests/queue)
