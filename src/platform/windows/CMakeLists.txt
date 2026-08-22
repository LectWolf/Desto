add_library(desto_platform_windows STATIC
    WindowsSingleInstanceGate.cpp
    WindowsSingleInstanceGate.h
    WindowsStartupIntegration.cpp
    WindowsStartupIntegration.h
    WindowsShellItemCatalog.cpp
    WindowsShellItemCatalog.h
    WindowsDisplayChangeSource.cpp
    WindowsDisplayChangeSource.h
    WindowsDirectoryChangeSource.cpp
    WindowsDirectoryChangeSource.h
    WindowsIconFont.cpp
    WindowsIconFont.h
    WindowsDesktopHost.cpp
    WindowsDesktopHost.h
    WindowsDesktopTriggerHost.cpp
    WindowsDesktopTriggerHost.h
    WindowsSettingsHost.cpp
    WindowsSettingsHost.h
    WindowsTextInput.cpp
    WindowsTextInput.h
    WindowsPopupMenu.cpp
    WindowsPopupMenu.h
    WindowsConfirmationDialog.cpp
    WindowsConfirmationDialog.h
    WindowsTrayHost.cpp
    WindowsTrayHost.h
    WindowsFileDragDrop.cpp
    WindowsFileDragDrop.h
    WindowsDisplayTopology.cpp
    WindowsDisplayTopology.h
)

target_compile_features(desto_platform_windows PUBLIC cxx_std_20)

target_compile_definitions(desto_platform_windows PRIVATE
    DESTO_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    DESTO_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    DESTO_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    DESTO_VERSION_BUILD=${DESTO_BUILD_NUMBER}
)

target_include_directories(desto_platform_windows
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(desto_platform_windows
    PUBLIC
        desto_platform
        desto_application
        desto_presentation
        desto_ui
        Comctl32
        Shcore
        User32
        Uxtheme
        Kernel32
        Gdi32
        Gdiplus
        Usp10
        Advapi32
        Wtsapi32
        Dwmapi
        Ole32
        OleAut32
        Taskschd
        Oleacc
        Propsys
        Shell32
        WindowsCodecs
)
