add_library(desto_platform_windows STATIC
    WindowsSingleInstanceGate.cpp
    WindowsSingleInstanceGate.h
    WindowsDisplayChangeSource.cpp
    WindowsDisplayChangeSource.h
    WindowsDesktopHost.cpp
    WindowsDesktopHost.h
    WindowsDisplayTopology.cpp
    WindowsDisplayTopology.h
)

target_compile_features(desto_platform_windows PUBLIC cxx_std_20)

target_include_directories(desto_platform_windows
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(desto_platform_windows
    PUBLIC
        desto_platform
        desto_application
        Shcore
        User32
        Kernel32
        Gdi32
)
