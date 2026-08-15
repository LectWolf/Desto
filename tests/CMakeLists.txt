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

add_test(NAME desto_display_topology_tests COMMAND desto_display_topology_tests)

add_executable(desto_storage_tests
    storage/StorageTests.cpp
)

target_include_directories(desto_storage_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(desto_storage_tests
    PRIVATE
        desto_storage
)

add_test(NAME desto_storage_tests COMMAND desto_storage_tests)
