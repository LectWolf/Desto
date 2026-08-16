if(NOT WIN32)
    return()
endif()

add_executable(desto_desktop_host WIN32
    DesktopHostMain.cpp
)

target_compile_features(desto_desktop_host PRIVATE cxx_std_20)

target_link_libraries(desto_desktop_host
    PRIVATE
        desto_application
        desto_platform_windows
        desto_storage
)
