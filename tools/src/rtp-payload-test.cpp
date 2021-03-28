#include <fmt/core.h>
#include <fmt/format.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <media_server/librtp/rtp-payload.h>
#include <media_server/librtp/rtp-profile.h>

using namespace gflags;
using namespace google;

DEFINE_string(rtpfile, "test.rtp", "rtp file to analysis");
DEFINE_string(outfile, "out.media", "media file to be saved");

struct rtp_payload_test_t
{
    int payload;
    const char *encoding;

    FILE *frtp;
    FILE *fsource2;

    void *decoder;

    size_t size;
    uint8_t packet[64 * 1024];
};

static void *rtp_alloc(void * /*param*/, int bytes)
{
    static uint8_t buffer[2 * 1024 * 1024 + 4] = {
        0,
        0,
        0,
        1,
    };
    LOG_IF(FATAL, bytes > sizeof(buffer) - 4)
        << "unexpected error, bytes too large";
    return buffer + 4;
}

static void rtp_free(void * /*param*/, void * /*packet*/) {}

static int rtp_decode_packet(void *param, const void *packet, int bytes,
                             uint32_t timestamp, int flags)
{
    static const uint8_t start_code[4] = {0, 0, 0, 1};
    struct rtp_payload_test_t *ctx = (struct rtp_payload_test_t *)param;

    static uint8_t buffer[2 * 1024 * 1024];
    LOG_IF(FATAL, bytes + 4 >= sizeof(buffer))
        << "unexpected error, bytes too large";
    // LOG_IF(FATAL, 0 != flags) << "unexpected error, unknown flags " << flags;

    size_t size = 0;
    if (0 == strcmp("H264", ctx->encoding) ||
        0 == strcmp("H265", ctx->encoding))
    {
        memcpy(buffer, start_code, sizeof(start_code));
        size += sizeof(start_code);
    }
    else if (0 == strcasecmp("mpeg4-generic", ctx->encoding))
    {
        int len = bytes + 7;
        uint8_t profile = 2;
        uint8_t sampling_frequency_index = 4;
        uint8_t channel_configuration = 2;
        buffer[0] = 0xFF; /* 12-syncword */
        buffer[1] = 0xF0 /* 12-syncword */ | (0 << 3) /*1-ID*/ |
                    (0x00 << 2) /*2-layer*/ | 0x01 /*1-protection_absent*/;
        buffer[2] = ((profile - 1) << 6) |
                    ((sampling_frequency_index & 0x0F) << 2) |
                    ((channel_configuration >> 2) & 0x01);
        buffer[3] =
            ((channel_configuration & 0x03) << 6) | ((len >> 11) & 0x03);
        /*0-original_copy*/                /*0-home*/
        /*0-copyright_identification_bit*/ /*0-copyright_identification_start*/
        buffer[4] = (uint8_t)(len >> 3);
        buffer[5] = ((len & 0x07) << 5) | 0x1F;
        buffer[6] = 0xFC | ((len / 1024) & 0x03);
        size = 7;
    }
    memcpy(buffer + size, packet, bytes);
    size += bytes;

    // TODO:
    // check media file
    fwrite(buffer, 1, size, ctx->fsource2);

    return 0;
}

void rtp_payload_test(int payload, const char *encoding, uint16_t seq,
                      uint32_t ssrc, const char *rtpfile, const char *outfile)
{
    LOG_IF(FATAL, access(rtpfile, F_OK) != 0)
        << "rtpfile " << rtpfile << " not exist";

    struct rtp_payload_test_t ctx;
    ctx.payload = payload;
    ctx.encoding = encoding;

    ctx.frtp = fopen(rtpfile, "rb");
    ctx.fsource2 = fopen(outfile, "wb");

    rtp_packet_setsize(1456); // 1456(live555)

    struct rtp_payload_t handler1;
    handler1.alloc = rtp_alloc;
    handler1.free = rtp_free;
    handler1.packet = rtp_decode_packet;
    ctx.decoder = rtp_payload_decode_create(payload, encoding, &handler1, &ctx);

    while (1)
    {
        LOG_EVERY_N(INFO, 1000) << "read from " << rtpfile;
        uint8_t s2[2];
        if (2 != fread(s2, 1, 2, ctx.frtp))
        {
            break;
        }

        ctx.size = (s2[0] << 8) | s2[1];
        LOG_IF(FATAL, ctx.size >= sizeof(ctx.packet))
            << "unexpected error, ctx size too large";
        if (ctx.size != (int)fread(ctx.packet, 1, ctx.size, ctx.frtp))
            break;

        rtp_payload_decode_input(ctx.decoder, ctx.packet, ctx.size);
    }

    fclose(ctx.frtp);
    fclose(ctx.fsource2);
    rtp_payload_decode_destroy(ctx.decoder);
}

int main(int argc, char **argv)
{
    FLAGS_logtostderr = 1;
    InitGoogleLogging(argv[0]);
    ParseCommandLineFlags(&argc, &argv, true);

    LOG(INFO) << "analysis rtp file " << FLAGS_rtpfile << "...";
    rtp_payload_test(33, "MP2T", 24470, 1726408532, FLAGS_rtpfile.c_str(), FLAGS_outfile.c_str());
    LOG(INFO) << "analysis rtp file " << FLAGS_rtpfile << " succ";
}
