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
endif()
