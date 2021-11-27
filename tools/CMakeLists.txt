# find_library(RTP_LIB librtp.a)
# find_library(RTSP_LIB librtsp.a)
# find_library(HTTP_LIB libhttp.a)

aux_source_directory(src sources)

add_executable(rtp-payload-test ${sources})

target_link_libraries(rtp-payload-test
    fmt
    gflags
    glog
    meida_server
    # ${RTP_LIB}
    # ${RTSP_LIB}
    # ${HTTP_LIB}
)

target_compile_features(rtp-payload-test PUBLIC cxx_std_17)
set_target_properties(rtp-payload-test PROPERTIES CXX_EXTENSIONS OFF)
