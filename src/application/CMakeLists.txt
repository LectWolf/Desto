add_library(desto_application STATIC
    ApplicationCardOrdering.cpp
    ApplicationCardOrdering.h
    ApplicationCardTransfer.cpp
    ApplicationCardTransfer.h
    ApplicationLifecycle.cpp
    ApplicationLifecycle.h
    ApplicationRuntime.cpp
    ApplicationRuntime.h
    Diagnostics.cpp
    Diagnostics.h
    SingleInstance.cpp
    SingleInstance.h
)

target_compile_features(desto_application PUBLIC cxx_std_20)

target_include_directories(desto_application
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(desto_application
    PUBLIC
        desto_domain
)
