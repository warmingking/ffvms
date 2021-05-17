find_library(HTTP_PARSER_LIB http_parser) # for libraries not managed by cmake, we prefer static library
find_library(PCAP_LIB pcap) # for libraries not managed by cmake, we prefer static library
find_library(RTP_LIB librtp.a)
find_library(RTSP_LIB librtsp.a)
find_library(HTTP_LIB libhttp.a)

aux_source_directory(src sources)

add_library(ffvms_core ${sources})

target_include_directories(ffvms_core
    PUBLIC include
)

target_link_libraries(ffvms_core
    fmt
    gflags
    glog
    event_core
    event_pthreads
    avformat
    avcodec
    avutil
    ${HTTP_PARSER_LIB}
    ${RTP_LIB}
    ${RTSP_LIB}
    ${HTTP_LIB}
    ${PCAP_LIB}
    grpc_common
    ffvms_api
)

target_compile_features(ffvms_core PUBLIC cxx_std_17)
set_target_properties(ffvms_core PROPERTIES CXX_EXTENSIONS OFF)

add_executable(ffvms ffvms.cpp)
target_link_libraries(ffvms ffvms_core)

enable_testing()

add_executable(network_server_ut unittest/network_server_ut.cpp)
target_link_libraries(network_server_ut ffvms_core gtest)

add_executable(rtp_producer_ut unittest/rtp_producer_ut.cpp)
target_link_libraries(rtp_producer_ut ffvms_core gtest)

add_executable(rtsp_service_ut unittest/rtsp_service_ut.cpp)
target_link_libraries(rtsp_service_ut ffvms_core gtest)