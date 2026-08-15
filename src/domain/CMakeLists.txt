add_library(desto_domain STATIC
    Card.cpp
    Card.h
)

target_compile_features(desto_domain PUBLIC cxx_std_20)

target_include_directories(desto_domain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

