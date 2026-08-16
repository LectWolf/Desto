add_library(desto_presentation STATIC
    CardContentLayout.cpp
    CardContentLayout.h
    CardView.cpp
    CardView.h
    PlacementInteraction.cpp
    PlacementInteraction.h
    PremultipliedImageResampler.cpp
    PremultipliedImageResampler.h
)

target_compile_features(desto_presentation PUBLIC cxx_std_20)

target_include_directories(desto_presentation
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(desto_presentation
    PUBLIC
        desto_domain
)
