add_library(endgate_brigadier STATIC
        reader.cpp
)
add_library(endgate::brigadier ALIAS endgate_brigadier)

target_include_directories(endgate_brigadier PUBLIC ${PROJECT_SOURCE_DIR}/src)
