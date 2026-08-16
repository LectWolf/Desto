cmake_minimum_required(VERSION 3.25)

project(Desto VERSION 0.1.0 LANGUAGES CXX)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DESTO_BUILD_TESTS "Build Desto tests" ON)
option(DESTO_BUILD_BENCHMARKS "Build headless performance benchmarks" ON)
option(DESTO_BUILD_PROTOTYPES "Build native UI prototypes" ON)
option(DESTO_BUILD_APPS "Build Desto desktop applications" ON)

add_subdirectory(src)

if(DESTO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(DESTO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(DESTO_BUILD_PROTOTYPES AND WIN32)
    add_subdirectory(prototypes)
endif()

if(DESTO_BUILD_APPS AND WIN32)
    add_subdirectory(apps)
endif()
