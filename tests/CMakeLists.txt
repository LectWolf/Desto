add_executable(desto_domain_tests
    domain/CardTests.cpp
)

target_link_libraries(desto_domain_tests
    PRIVATE
        desto_domain
)

add_test(NAME desto_domain_tests COMMAND desto_domain_tests)

