add_library(desto_storage STATIC
    ApplicationCardImport.cpp
    ApplicationCardImport.h
    ApplicationCardReturn.cpp
    ApplicationCardReturn.h
    DirectoryImportPlanner.cpp
    DirectoryImportPlanner.h
    FileMoveTransaction.cpp
    FileMoveTransaction.h
    JsonConfigStore.cpp
    JsonConfigStore.h
    MappingRegistry.h
    MappingCardImport.cpp
    MappingCardImport.h
    StorageRoot.cpp
    StorageRoot.h
    StorageRootMigration.cpp
    StorageRootMigration.h
)

target_compile_features(desto_storage PUBLIC cxx_std_20)

target_include_directories(desto_storage
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
)

target_link_libraries(desto_storage
    PUBLIC
        desto_domain
)
