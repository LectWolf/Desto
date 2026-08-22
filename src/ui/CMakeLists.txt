add_library(desto_ui STATIC
    Dialog.cpp
    Dialog.h
    RecoveryView.cpp
    RecoveryView.h
    TextInputModel.cpp
    TextInputModel.h
)

target_compile_features(desto_ui PUBLIC cxx_std_20)

target_include_directories(desto_ui
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)
