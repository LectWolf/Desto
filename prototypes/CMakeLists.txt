add_executable(desto_native_prototype WIN32
    NativePrototype.cpp
)

target_compile_features(desto_native_prototype PRIVATE cxx_std_20)

target_link_libraries(desto_native_prototype
    PRIVATE
        desto_application
        desto_platform_windows
        User32
        Gdi32
    )
