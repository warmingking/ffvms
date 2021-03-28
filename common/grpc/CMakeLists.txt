aux_source_directory(src sources)

add_library(grpc_common ${sources})

target_include_directories(grpc_common
    PUBLIC include
)

target_link_libraries(grpc_common
    gRPC::grpc++
    # protobuf::libprotobuf
)

set(_gRPC_PROTO_GENS_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen-cpp)

enable_testing()

find_package(gRPC QUIET)
find_package(Protobuf QUIET)
find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

protobuf_generate_grpc_cpp(
    GRPC_VIDEO_GREETER_SRCS
    GRPC_VIDEO_GREETER_HDRS
    ${_gRPC_PROTO_GENS_DIR}
    ./unittest/greeter.proto
)

add_executable(async_client_ut
    unittest/async_client_ut.cpp
    ${_gRPC_PROTO_GENS_DIR}/greeter.grpc.pb.cc
    ${_gRPC_PROTO_GENS_DIR}/greeter.pb.cc
)

target_include_directories(async_client_ut
    PUBLIC ${_gRPC_PROTO_GENS_DIR}
)

target_link_libraries(async_client_ut
    gtest
    glog
    gflags
    fmt
    grpc_common
)