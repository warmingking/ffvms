find_package(gRPC QUIET)
find_package(Protobuf QUIET)
find_program(PROTOC_EXECUTABLE protoc)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)

set(_gRPC_PROTO_GENS_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen-cpp)

protobuf_generate_grpc_cpp(
    GRPC_VIDEO_GREETER_SRCS
    GRPC_VIDEO_GREETER_HDRS
    ${_gRPC_PROTO_GENS_DIR}
    ./video_greeter.proto
)

add_library(ffvms_api
    ${_gRPC_PROTO_GENS_DIR}/video_greeter.grpc.pb.cc
    ${_gRPC_PROTO_GENS_DIR}/video_greeter.pb.cc
)

target_include_directories(ffvms_api
    PUBLIC ${_gRPC_PROTO_GENS_DIR}
)

target_link_libraries(ffvms_api
    gRPC::grpc++
    gRPC::grpc++_reflection
    protobuf::libprotobuf
)