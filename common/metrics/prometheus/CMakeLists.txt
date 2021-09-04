aux_source_directory(src sources)

add_library(prometheus_common ${sources})

target_include_directories(prometheus_common
    PUBLIC include
)

target_link_libraries(prometheus_common
    # prometheus deps
    pthread
    prometheus-cpp-pull
    prometheus-cpp-core
    z
    # mm deps
    fmt
)

enable_testing()
include(GoogleTest)

add_executable(metrics_manager_ut
    unittest/metrics_manager_ut.cpp
)

target_link_libraries(metrics_manager_ut
    gtest
    glog
    gflags
    prometheus_common
)

gtest_discover_tests(metrics_manager_ut)