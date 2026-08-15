cmake_minimum_required(VERSION 3.25)

project(Desto VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DESTO_BUILD_TESTS "Build Desto tests" ON)

add_subdirectory(src)

if(DESTO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

