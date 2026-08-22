add_executable(desto_headless_baseline
    HeadlessBaseline.cpp
)

target_compile_features(desto_headless_baseline PRIVATE cxx_std_20)

target_link_libraries(desto_headless_baseline
    PRIVATE
        desto_application
        desto_storage
)

if(WIN32)
    target_link_libraries(desto_headless_baseline PRIVATE Psapi)

    add_executable(desto_stability_soak
        StabilitySoak.cpp
    )

    target_compile_features(desto_stability_soak PRIVATE cxx_std_20)

    target_link_libraries(desto_stability_soak
        PRIVATE
            desto_platform_windows
            desto_storage
            Psapi
    )

    add_executable(desto_desktop_performance_gate
        DesktopPerformanceGate.cpp
    )

    target_compile_features(desto_desktop_performance_gate PRIVATE cxx_std_20)

    target_link_libraries(desto_desktop_performance_gate
        PRIVATE
            desto_platform_windows
            Psapi
    )

endif()
