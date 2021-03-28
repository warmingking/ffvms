cmake_minimum_required(VERSION 3.13)
project(ffvms)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(protobuf_generate_grpc_cpp SRCS HDRS GENERATED_DIR)
  if(NOT ARGN)
    message(SEND_ERROR "Error: protobuf_generate_grpc_cpp called without any proto files")
    return()
  endif()

  file(MAKE_DIRECTORY ${GENERATED_DIR})

  if(PROTOBUF_GENERATE_CPP_APPEND_PATH) # This variable is common for all types of output.
    # Create an include path for each file specified
    foreach(FIL ${ARGN})
      get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
      get_filename_component(ABS_PATH ${ABS_FIL} PATH)
      list(FIND _protobuf_include_path ${ABS_PATH} _contains_already)
      if(${_contains_already} EQUAL -1)
          list(APPEND _protobuf_include_path -I ${ABS_PATH})
      endif()
    endforeach()
  else()
    set(_protobuf_include_path -I ${CMAKE_CURRENT_SOURCE_DIR})
  endif()

  if(DEFINED PROTOBUF_IMPORT_DIRS)
    foreach(DIR ${Protobuf_IMPORT_DIRS})
      get_filename_component(ABS_PATH ${DIR} ABSOLUTE)
      list(FIND _protobuf_include_path ${ABS_PATH} _contains_already)
      if(${_contains_already} EQUAL -1)
          list(APPEND _protobuf_include_path -I ${ABS_PATH})
      endif()
    endforeach()
  endif()

  set(${SRCS})
  set(${HDRS})
  foreach(FIL ${ARGN})
    get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
    get_filename_component(FIL_WE ${FIL} NAME_WE)

    list(APPEND ${SRCS} "${GENERATED_DIR}/${FIL_WE}.pb.cc")
    list(APPEND ${SRCS} "${GENERATED_DIR}/${FIL_WE}.grpc.pb.cc")
    list(APPEND ${HDRS} "${GENERATED_DIR}/${FIL_WE}.pb.h")
    list(APPEND ${HDRS} "${GENERATED_DIR}/${FIL_WE}.grpc.pb.h")

    add_custom_command(
      OUTPUT "${GENERATED_DIR}/${FIL_WE}.pb.cc"
             "${GENERATED_DIR}/${FIL_WE}.grpc.pb.cc"
             "${GENERATED_DIR}/${FIL_WE}.pb.h"
             "${GENERATED_DIR}/${FIL_WE}.grpc.pb.h"
      COMMAND ${PROTOC_EXECUTABLE}
      ARGS --grpc_out=${GENERATED_DIR}
           --cpp_out=${GENERATED_DIR}
           --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
           ${_protobuf_include_path} ${ABS_FIL}
      DEPENDS ${ABS_FIL} ${PROTOC_EXECUTABLE}
      COMMENT "Running gRPC C++ protocol buffer compiler on ${FIL}"
      VERBATIM)
  endforeach()

  set_source_files_properties(${${SRCS}} ${${HDRS}} PROPERTIES GENERATED TRUE)
  set(${SRCS} ${${SRCS}} PARENT_SCOPE)
  set(${HDRS} ${${HDRS}} PARENT_SCOPE)
  include_directories(${GENERATED_DIR})
endfunction()

add_subdirectory(common)
add_subdirectory(api)
add_subdirectory(core)
add_subdirectory(tools)