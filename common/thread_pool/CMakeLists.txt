add_library(thread_pool_common INTERFACE)

target_include_directories(thread_pool_common
    INTERFACE include
)

target_link_libraries(thread_pool_common
    INTERFACE pthread
)

enable_testing()
include(GoogleTest)

add_executable(thread_pool_ut unittest/thread_pool_ut.cpp)
target_link_libraries(thread_pool_ut
    gtest
    glog
    gflags
    thread_pool_common
)

gtest_discover_tests(thread_pool_ut)