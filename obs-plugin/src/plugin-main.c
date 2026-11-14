#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/platform.h>
#include <util/threading.h>
#include <plugin-support.h>

#include <libavcodec/avcodec.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define PHONE_PORT 5000
#define READ_SIZE 65536

struct phonecam_source {
    obs_source_t *source;
    pthread_t thread;
    bool thread_started;
    volatile bool stop;
    bool frame_logged;
    bool format_warned;
};

static int connect_to_phone(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PHONE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void output_frame(struct phonecam_source *s, AVFrame *f)
{
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
        if (!s->format_warned) {
            obs_log(LOG_WARNING, "unsupported pixel format %d", f->format);
            s->format_warned = true;
        }
        return;
    }

    bool full = (f->format == AV_PIX_FMT_YUVJ420P) || (f->color_range == AVCOL_RANGE_JPEG);

    struct obs_source_frame out;
    memset(&out, 0, sizeof(out));
    out.width = (uint32_t)f->width;
    out.height = (uint32_t)f->height;
    out.format = VIDEO_FORMAT_I420;
    for (int i = 0; i < 3; i++) {
        out.data[i] = f->data[i];
        out.linesize[i] = (uint32_t)f->linesize[i];
    }
    out.timestamp = os_gettime_ns();
    out.full_range = full;
    video_format_get_parameters_for_format(f->height >= 720 ? VIDEO_CS_709 : VIDEO_CS_601,
                                           full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
                                           VIDEO_FORMAT_I420, out.color_matrix,
                                           out.color_range_min, out.color_range_max);
    obs_source_output_video(s->source, &out);

    if (!s->frame_logged) {
        obs_log(LOG_INFO, "first frame received: %dx%d", f->width, f->height);
        s->frame_logged = true;
    }
}

static void decode_packet(struct phonecam_source *s, AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame)
{
    if (avcodec_send_packet(ctx, pkt) < 0) {
        return;
    }
    while (avcodec_receive_frame(ctx, frame) == 0) {
        output_frame(s, frame);
        av_frame_unref(frame);
    }
}

static void run_session(struct phonecam_source *s, int fd)
{
    s->frame_logged = false;

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        obs_log(LOG_ERROR, "H.264 decoder not found");
        return;
    }

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *buf = bmalloc(READ_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);

    if (!ctx || !parser || !pkt || !frame || !buf) {
        obs_log(LOG_ERROR, "could not allocate decoder");
        goto cleanup;
    }

    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags2 |= AV_CODEC_FLAG2_FAST;
    ctx->thread_count = 1;

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        obs_log(LOG_ERROR, "could not open H.264 decoder");
        goto cleanup;
    }

    while (!os_atomic_load_bool(&s->stop)) {
        ssize_t n = recv(fd, buf, READ_SIZE, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            break;
        }

        memset(buf + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        uint8_t *data = buf;
        int size = (int)n;

        while (size > 0) {
            int used = av_parser_parse2(parser, ctx, &pkt->data, &pkt->size, data, size,
                                        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) {
                break;
            }
            data += used;
            size -= used;
            if (pkt->size > 0) {
                decode_packet(s, ctx, pkt, frame);
            }
        }
    }

cleanup:
    obs_source_output_video(s->source, NULL);
    if (parser) {
        av_parser_close(parser);
    }
    if (ctx) {
        avcodec_free_context(&ctx);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    bfree(buf);
}

static void *phonecam_thread(void *data)
{
    struct phonecam_source *s = data;

    while (!os_atomic_load_bool(&s->stop)) {
        int fd = connect_to_phone();
        if (fd < 0) {
            os_sleep_ms(500);
            continue;
        }
        run_session(s, fd);
        close(fd);
        os_sleep_ms(300);
    }
    return NULL;
}

static const char *phonecam_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "OpenPhoneCam";
}

static void *phonecam_create(obs_data_t *settings, obs_source_t *source)
{
    UNUSED_PARAMETER(settings);

    struct phonecam_source *s = bzalloc(sizeof(struct phonecam_source));
    s->source = source;
    obs_source_set_async_unbuffered(source, true);
    os_atomic_set_bool(&s->stop, false);

    if (pthread_create(&s->thread, NULL, phonecam_thread, s) == 0) {
        s->thread_started = true;
    } else {
        obs_log(LOG_ERROR, "could not start the receiver thread");
    }
    return s;
}

static void phonecam_destroy(void *data)
{
    struct phonecam_source *s = data;

    os_atomic_set_bool(&s->stop, true);
    if (s->thread_started) {
        pthread_join(s->thread, NULL);
    }
    bfree(s);
}

static struct obs_source_info phonecam_info = {
    .id = "openphonecam_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .icon_type = OBS_ICON_TYPE_CAMERA,
    .get_name = phonecam_get_name,
    .create = phonecam_create,
    .destroy = phonecam_destroy,
};

bool obs_module_load(void)
{
    unsigned int v = avcodec_version();
    obs_register_source(&phonecam_info);
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    obs_log(LOG_INFO, "libavcodec %u.%u.%u", v >> 16, (v >> 8) & 0xff, v & 0xff);
    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "plugin unloaded");
}
