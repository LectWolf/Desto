if(NOT WIN32)
    return()
endif()

add_executable(desto_desktop_host WIN32
    DesktopHostMain.cpp
    Desto.rc
)

target_compile_features(desto_desktop_host PRIVATE cxx_std_20)

target_compile_definitions(desto_desktop_host PRIVATE
    DESTO_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DESTO_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DESTO_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    DESTO_VERSION_BUILD=${DESTO_BUILD_NUMBER}
)

target_link_libraries(desto_desktop_host
    PRIVATE
        desto_application
        desto_platform_windows
        desto_storage
)

set_target_properties(desto_desktop_host PROPERTIES
    OUTPUT_NAME "Desto"
)
