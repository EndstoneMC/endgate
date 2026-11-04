add_library(endgate_bstats STATIC
        base.cpp
        charts.cpp
        config.cpp
)
add_library(endgate::bstats ALIAS endgate_bstats)

target_include_directories(endgate_bstats PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(endgate_bstats
        PUBLIC endgate::http glaze::glaze
        PRIVATE endgate::uuid libdeflate::libdeflate
)
