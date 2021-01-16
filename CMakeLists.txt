cmake_minimum_required(VERSION 3.15)
project(ffvms)
include(FetchContent)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(FETCHCONTENT_QUIET OFF)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")

find_package(FFMPEG QUIET)

FetchContent_Declare(
    gflags
    GIT_REPOSITORY https://github.com/gflags/gflags
    GIT_TAG        v2.2.2
)

FetchContent_Declare(
    glog
    GIT_REPOSITORY https://github.com/google/glog
    GIT_TAG        v0.4.0
    PATCH_COMMAND  git apply "${CMAKE_CURRENT_BINARY_DIR}/patches/glog.patch"
)

FetchContent_Declare(
    libevent
    GIT_REPOSITORY https://github.com/libevent/libevent
    GIT_TAG        release-2.1.12-stable
)

FetchContent_Declare(
    gRPC
    GIT_REPOSITORY https://github.com/grpc/grpc
    GIT_TAG        v1.34.1
)

FetchContent_MakeAvailable(gflags glog gRPC libevent)
target_include_directories(glog PRIVATE "${gflags_BINARY_DIR}/include")

FetchContent_Declare(
    ctpl
    GIT_REPOSITORY https://github.com/vit-vit/CTPL.git
    GIT_TAG        ctpl_v.0.0.2
)

FetchContent_GetProperties(ctpl)
if (NOT ctpl_POPULATED)
  FetchContent_Populate(ctpl)
endif ()

FetchContent_Declare(
    http-parser
    GIT_REPOSITORY https://github.com/ploxiln/http-parser
    GIT_TAG        master
)

FetchContent_GetProperties(http-parser)
if (NOT http-parser_POPULATED)
  FetchContent_Populate(http-parser)
endif ()

add_custom_command(
    OUTPUT ${http-parser_SOURCE_DIR}/libhttp_parser.a
    COMMAND make
    ARGS package
    WORKING_DIRECTORY ${http-parser_SOURCE_DIR}
    COMMENT "build http-parser"
    VERBATIM
)
add_custom_target(http-parser-package DEPENDS ${http-parser_SOURCE_DIR}/libhttp_parser.a)
add_library(http-parser STATIC IMPORTED)
set_target_properties(http-parser PROPERTIES
    IMPORTED_LOCATION ${http-parser_SOURCE_DIR}/libhttp_parser.a)
add_dependencies(http-parser http-parser-package)

set(_PROTOBUF_LIBPROTOBUF libprotobuf)
set(_REFLECTION grpc++_reflection)
set(_PROTOBUF_PROTOC $<TARGET_FILE:protoc>)

set(_GRPC_GRPCPP grpc++)
set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)

# Proto file
get_filename_component(gb_proto "./java/ffvms-contract/src/main/proto/sipper.proto" ABSOLUTE)
get_filename_component(gb_proto_path "${gb_proto}" PATH)

# Generated sources
set(proto_gens "${CMAKE_CURRENT_BINARY_DIR}/gens")
set(gb_proto_srcs "${proto_gens}/sipper.pb.cc")
set(gb_proto_hdrs "${proto_gens}/sipper.pb.h")
set(gb_grpc_srcs "${proto_gens}/sipper.grpc.pb.cc")
set(gb_grpc_hdrs "${proto_gens}/sipper.grpc.pb.h")

add_custom_command(
    OUTPUT "${gb_proto_srcs}" "${gb_proto_hdrs}" "${gb_grpc_srcs}" "${gb_grpc_hdrs}"
    COMMAND ${_PROTOBUF_PROTOC}
    ARGS --grpc_out "${proto_gens}"
         --cpp_out "${proto_gens}"
         -I "${gb_proto_path}"
         --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}" "${gb_proto}"
    DEPENDS "${gb_proto}")

# Include generated *.pb.h files

include_directories("${CMAKE_CURRENT_BINARY_DIR}" "${ctpl_SOURCE_DIR}" "${http-parser_SOURCE_DIR}")

# Targets greeter_[async_](client|server)
add_executable(ffvms 
    ffvms.cpp
    rtsp_server.cpp
    video_manager_service.cpp
    gb_manager_service.cpp
    rtsp_parser.cpp
    common.cpp
    ${gb_proto_srcs}
    ${gb_grpc_srcs})

target_link_libraries(ffvms
    gflags
    glog
    event_core
    event_pthreads
    avutil
    avformat
    avcodec
    http-parser
    ${_REFLECTION}
    ${_GRPC_GRPCPP}
    ${_PROTOBUF_LIBPROTOBUF})
