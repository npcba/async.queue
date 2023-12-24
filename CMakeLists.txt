cmake_minimum_required(VERSION 3.16)

project (async CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus)
    add_definitions(-D_WIN32_WINNT=0x0603) # Windows 8.1
else()
    add_compile_options(-Wall -Wextra -Wno-deprecated-declarations -Wno-comments -Wsuggest-override -Werror)
endif()

if (NOT DEFINED Boost_FOUND)
    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost COMPONENTS system context coroutine REQUIRED)
endif()

add_subdirectory(include)
add_subdirectory(tests/queue)
