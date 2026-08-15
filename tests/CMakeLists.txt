add_executable(desto_domain_tests
    domain/CardTests.cpp
)

target_link_libraries(desto_domain_tests
    PRIVATE
        desto_domain
)

add_test(NAME desto_domain_tests COMMAND desto_domain_tests)

add_executable(desto_storage_tests
    storage/StorageTests.cpp
)

target_link_libraries(desto_storage_tests
    PRIVATE
        desto_storage
)

add_test(NAME desto_storage_tests COMMAND desto_storage_tests)
