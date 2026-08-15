add_library(desto_platform STATIC
    DisplayTopology.cpp
    DisplayTopology.h
)

target_compile_features(desto_platform PUBLIC cxx_std_20)

target_include_directories(desto_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(desto_platform
    PUBLIC
        desto_domain
)

if(WIN32)
    add_subdirectory(windows)
endif()
