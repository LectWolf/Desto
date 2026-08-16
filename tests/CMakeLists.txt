add_executable(desto_domain_tests
    domain/CardTests.cpp
)

target_include_directories(desto_domain_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_domain_tests
    PRIVATE
        desto_domain
)

add_test(NAME desto_domain_tests COMMAND desto_domain_tests)

add_executable(desto_workspace_layout_tests
    domain/WorkspaceLayoutTests.cpp
)

target_include_directories(desto_workspace_layout_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_workspace_layout_tests
    PRIVATE
        desto_domain
)

add_test(NAME desto_workspace_layout_tests COMMAND desto_workspace_layout_tests)

add_executable(desto_display_topology_tests
    platform/DisplayTopologyTests.cpp
)

target_include_directories(desto_display_topology_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_display_topology_tests
    PRIVATE
        desto_platform
)

if(WIN32)
    target_link_libraries(desto_display_topology_tests
        PRIVATE
            desto_platform_windows
    )

    add_executable(desto_windows_single_instance_tests
        platform/WindowsSingleInstanceTests.cpp
    )

    target_include_directories(desto_windows_single_instance_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(desto_windows_single_instance_tests
        PRIVATE
            desto_platform_windows
    )

    add_test(NAME desto_windows_single_instance_tests COMMAND desto_windows_single_instance_tests)

    add_executable(desto_windows_desktop_host_tests
        platform/WindowsDesktopHostTests.cpp
    )

    target_include_directories(desto_windows_desktop_host_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(desto_windows_desktop_host_tests
        PRIVATE
            desto_platform_windows
    )

    add_test(NAME desto_windows_desktop_host_tests COMMAND desto_windows_desktop_host_tests)

    add_executable(desto_windows_file_drag_drop_tests
        platform/WindowsFileDragDropTests.cpp
    )

    target_include_directories(desto_windows_file_drag_drop_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(desto_windows_file_drag_drop_tests
        PRIVATE
            desto_platform_windows
    )

    add_test(NAME desto_windows_file_drag_drop_tests COMMAND desto_windows_file_drag_drop_tests)

    add_executable(desto_windows_shell_item_catalog_tests
        platform/WindowsShellItemCatalogTests.cpp
    )

    target_include_directories(desto_windows_shell_item_catalog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    target_link_libraries(desto_windows_shell_item_catalog_tests
        PRIVATE
            desto_platform_windows
    )

    add_test(NAME desto_windows_shell_item_catalog_tests COMMAND desto_windows_shell_item_catalog_tests)
endif()

add_test(NAME desto_display_topology_tests COMMAND desto_display_topology_tests)

add_executable(desto_application_runtime_tests
    application/ApplicationRuntimeTests.cpp
)

target_include_directories(desto_application_runtime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_application_runtime_tests
    PRIVATE
        desto_application
)

add_test(NAME desto_application_runtime_tests COMMAND desto_application_runtime_tests)

add_executable(desto_application_card_transfer_tests
    application/ApplicationCardTransferTests.cpp
)

target_include_directories(desto_application_card_transfer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_application_card_transfer_tests
    PRIVATE
        desto_application
)

add_test(NAME desto_application_card_transfer_tests COMMAND desto_application_card_transfer_tests)

add_executable(desto_application_card_ordering_tests
    application/ApplicationCardOrderingTests.cpp
)

target_include_directories(desto_application_card_ordering_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_application_card_ordering_tests
    PRIVATE
        desto_application
)

add_test(NAME desto_application_card_ordering_tests COMMAND desto_application_card_ordering_tests)

add_executable(desto_application_lifecycle_tests
    application/ApplicationLifecycleTests.cpp
)

target_include_directories(desto_application_lifecycle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_application_lifecycle_tests
    PRIVATE
        desto_application
)

add_test(NAME desto_application_lifecycle_tests COMMAND desto_application_lifecycle_tests)

add_executable(desto_storage_tests
    storage/StorageTests.cpp
)

target_include_directories(desto_storage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_storage_tests
    PRIVATE
        desto_storage
        desto_application
)

add_test(NAME desto_storage_tests COMMAND desto_storage_tests)

add_executable(desto_application_card_import_tests
    storage/ApplicationCardImportTests.cpp
)

target_include_directories(desto_application_card_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_application_card_import_tests
    PRIVATE
        desto_storage
)

add_test(NAME desto_application_card_import_tests COMMAND desto_application_card_import_tests)

add_executable(desto_card_view_tests
    presentation/CardViewTests.cpp
)

target_include_directories(desto_card_view_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_card_view_tests
    PRIVATE
        desto_presentation
)

add_test(NAME desto_card_view_tests COMMAND desto_card_view_tests)

add_executable(desto_card_content_layout_tests
    presentation/CardContentLayoutTests.cpp
)

target_include_directories(desto_card_content_layout_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_card_content_layout_tests
    PRIVATE
        desto_presentation
)

add_test(NAME desto_card_content_layout_tests COMMAND desto_card_content_layout_tests)

add_executable(desto_placement_interaction_tests
    presentation/PlacementInteractionTests.cpp
)

target_include_directories(desto_placement_interaction_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_placement_interaction_tests
    PRIVATE
        desto_presentation
)

add_test(NAME desto_placement_interaction_tests COMMAND desto_placement_interaction_tests)
