# sdk
list(APPEND sdk_include
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libaio/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libhttp/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libice/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libsdk/include"
)

list(APPEND sdk_library
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libaio/release.linux/libaio.so"
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libhttp/release.linux/libhttp.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libice/release.linux/libice.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/sdk/libsdk/release.linux/libsdk.a"
)

add_custom_command(
     OUTPUT ${sdk_library}
     COMMAND make -j4 RELEASE=1
     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/sdk
)

add_custom_target(sdk_target DEPENDS ${sdk_library})

# avcodec
list(APPEND avcodec_include
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/avbsf/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/avcodec/include"
     # "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/avplayer/include"
     # "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/ffutils/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/h264/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/h265/include"
     # "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/libavo/include"
)

list(APPEND avcodec_library
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/avbsf/release.linux/libavbsf.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/avcodec/release.linux/libavcodec.a"
     # "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/avplayer/release.linux/libavplayer.a"
     # "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/ffutils/release.linux/libffutils.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/h264/release.linux/libh264.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/h265/release.linux/libh265.a"
     # "${CMAKE_CURRENT_SOURCE_DIR}/avcodec/libavo/release.linux/libavo.so"
)

add_custom_command(
     OUTPUT ${avcodec_library}
     COMMAND git checkout -- .
             && git apply ${CMAKE_CURRENT_SOURCE_DIR}/media-server-avcodec.patch
             && make -j4 RELEASE=1
     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/avcodec
)

add_custom_target(avcodec_target DEPENDS ${avcodec_library})

# server
list(APPEND server_include
     "${CMAKE_CURRENT_SOURCE_DIR}/meida-server/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libdash/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libflv/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libhls/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libmov/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libmpeg/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/librtmp/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/librtp/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/librtsp/include"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libsip/include"
)

list(APPEND server_library
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libdash/release.linux/libdash.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libflv/release.linux/libflv.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libhls/release.linux/libhls.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libmov/release.linux/libmov.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libmpeg/release.linux/libmpeg.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/librtmp/release.linux/librtmp.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/librtp/release.linux/librtp.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/librtsp/release.linux/librtsp.a"
     "${CMAKE_CURRENT_SOURCE_DIR}/media-server/libsip/release.linux/libsip.a"
)

add_custom_command(
     OUTPUT ${server_library}
     COMMAND make -j4 RELEASE=1
     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/media-server
)

add_custom_target(server_target DEPENDS ${server_library})

add_library(media_server INTERFACE)
target_link_libraries(media_server INTERFACE
     ${sdk_library}
     ${avcodec_library}
     ${server_library}
)
target_include_directories(media_server INTERFACE
     ${sdk_include}
     ${avcodec_include}
     ${server_include}
)
add_dependencies(media_server
     sdk_target
     avcodec_target
     server_target
)