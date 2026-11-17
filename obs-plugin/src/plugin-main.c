#include <obs-module.h>
#include <media-io/video-io.h>
#include <util/platform.h>
#include <util/threading.h>
#include <plugin-support.h>

#include <libavcodec/avcodec.h>
#include <libavutil/error.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define DEFAULT_PORT 5000
#define SETTING_PORT "port"
#define SETTING_MODE "connection_mode"
#define SETTING_ADDRESS "address"
#define SETTING_ADB "adb_path"
#define SETTING_LATENCY "latency_mode"

#define CONNECTION_USB 0
#define CONNECTION_WIFI 1

#define LATENCY_LOWEST 0
#define LATENCY_SMOOTH 1

#define READ_SIZE 65536
#define FORWARD_RETRY_NS 2000000000ULL

#define UDP_MAGIC 0x4F50434Du
#define UDP_HEADER_SIZE 12
#define UDP_MAX_PAYLOAD 1400
#define FEC_K 8
#define FEC_BLOCK (FEC_K + 1)
#define RING_SIZE 1024
#define BLOCK_SLOTS 64
#define STALL_TIMEOUT_NS 250000000LL

struct lensgo_source {
    obs_source_t *source;
    pthread_t thread;
    bool thread_started;
    volatile bool stop;
    volatile long port;
    volatile long mode;
    pthread_mutex_t lock;
    char address[128];
    char adb_path[1024];
    uint64_t last_forward_ns;
    uint64_t last_connect_log_ns;
    bool forward_ready_logged;
    bool forward_failed_logged;
    bool warned_no_address;
    bool format_warned;
    uint64_t bytes;
    uint64_t packets;
    uint64_t frames;
    uint64_t decode_errors;
};

struct udp_header {
    uint32_t magic;
    uint32_t seq;
    uint8_t flags;
    uint16_t payload_len;
};

struct chunk {
    bool valid;
    uint32_t seq;
    uint16_t len;
    uint8_t data[UDP_MAX_PAYLOAD];
};

struct block_slot {
    int32_t block_id;
    uint16_t mask;
    uint8_t data[FEC_K][UDP_MAX_PAYLOAD];
    bool dvalid[FEC_K];
    uint8_t parity[UDP_MAX_PAYLOAD];
    bool pvalid;
};

struct udp_ctx {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;
    AVPacket *pkt;
    AVFrame *frame;
    uint8_t *detect_buf;
    size_t detect_len;
    bool codec_ready;
    struct chunk ring[RING_SIZE];
    uint32_t next_seq;
    struct block_slot blocks[BLOCK_SLOTS];
    uint64_t stall_ns;
    struct lensgo_source *s;
};

static bool file_is_executable(const char *path)
{
    return path != NULL && path[0] != '\0' && access(path, X_OK) == 0;
}

static const char *find_adb(const char *override)
{
    static char path[1024];

    if (file_is_executable(override)) {
        return override;
    }

    const char *sdk = getenv("ANDROID_HOME");
    if (!sdk || !sdk[0]) {
        sdk = getenv("ANDROID_SDK_ROOT");
    }
    if (sdk && sdk[0]) {
        snprintf(path, sizeof(path), "%s/platform-tools/adb", sdk);
        if (file_is_executable(path)) {
            return path;
        }
    }

    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/Library/Android/sdk/platform-tools/adb", home);
        if (file_is_executable(path)) {
            return path;
        }
        snprintf(path, sizeof(path), "%s/Android/Sdk/platform-tools/adb", home);
        if (file_is_executable(path)) {
            return path;
        }
    }

    static const char *fixed[] = {
        "/opt/homebrew/bin/adb",
        "/usr/local/bin/adb",
        "/usr/bin/adb",
        "/bin/adb",
    };
    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        if (file_is_executable(fixed[i])) {
            snprintf(path, sizeof(path), "%s", fixed[i]);
            return path;
        }
    }

    return NULL;
}

static bool run_adb_forward(const char *override, int port)
{
    const char *adb = find_adb(override);
    const char *exe = adb ? adb : "adb";

    char forward[32];
    snprintf(forward, sizeof(forward), "tcp:%d", port);

    char *argv[8];
    int i = 0;
    argv[i++] = (char *)exe;
    argv[i++] = (char *)"forward";
    argv[i++] = forward;
    argv[i++] = forward;
    argv[i] = NULL;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = 0;
    int rc = posix_spawnp(&pid, exe, &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0) {
        return false;
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        /* The host may reap children itself (SIGCHLD ignored); treat as spawned. */
        return errno == ECHILD;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int connect_to_address(const char *host, int port)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return -1;
    }

    int rcvbuf = 262144;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

static void output_frame(struct lensgo_source *s, AVFrame *f)
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

    if (s->frames == 0) {
        obs_log(LOG_INFO, "first frame received: %dx%d", f->width, f->height);
    }
    s->frames++;
}

static void decode_packet(struct lensgo_source *s, AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame)
{
    int ret = avcodec_send_packet(ctx, pkt);
    if (ret < 0) {
        if (s->decode_errors < 5) {
            char err[128];
            av_strerror(ret, err, sizeof(err));
            obs_log(LOG_WARNING, "send_packet failed: %s", err);
        }
        s->decode_errors++;
        return;
    }
    while (avcodec_receive_frame(ctx, frame) == 0) {
        output_frame(s, frame);
        av_frame_unref(frame);
    }
}

static int find_start_code(const uint8_t *d, int n)
{
    for (int i = 0; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            return i;
        }
        if (i + 4 < n && d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1) {
            return i;
        }
    }
    return -1;
}

static enum AVCodecID detect_codec(const uint8_t *d, int n)
{
    int sc = find_start_code(d, n);
    if (sc < 0) {
        return AV_CODEC_ID_NONE;
    }

    int p = sc;
    if (p + 3 < n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 1) {
        p += 4;
    } else {
        p += 3;
    }
    if (p >= n) {
        return AV_CODEC_ID_NONE;
    }

    uint8_t b = d[p];
    int h264_type = b & 0x1F;
    int hevc_type = (b >> 1) & 0x3F;

    if (hevc_type >= 32 && hevc_type <= 34) {
        return AV_CODEC_ID_HEVC;
    }
    if (h264_type == 7 || h264_type == 8 || h264_type == 5) {
        return AV_CODEC_ID_H264;
    }
    if (hevc_type >= 16 && hevc_type <= 21) {
        return AV_CODEC_ID_HEVC;
    }
    if (h264_type >= 1 && h264_type <= 23) {
        return AV_CODEC_ID_H264;
    }
    return AV_CODEC_ID_NONE;
}

static bool udp_parse_header(const uint8_t *p, size_t n, struct udp_header *h)
{
    if (n < UDP_HEADER_SIZE) return false;
    uint32_t magic = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    h->magic = magic;
    h->seq = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) | ((uint32_t)p[6] << 8) | (uint32_t)p[7];
    h->flags = p[8];
    h->payload_len = ((uint16_t)p[9] << 8) | (uint16_t)p[10];
    return true;
}

static void udp_free_ctx(struct udp_ctx *c)
{
    if (c->pkt) av_packet_free(&c->pkt);
    if (c->frame) av_frame_free(&c->frame);
    if (c->detect_buf) bfree(c->detect_buf);
    if (c->parser) av_parser_close(c->parser);
    if (c->ctx) avcodec_free_context(&c->ctx);
    bfree(c);
}

static void udp_feed(struct udp_ctx *c, const uint8_t *data, int len)
{
    if (!c->codec_ready) {
        if (c->detect_len + (size_t)len > READ_SIZE) {
            size_t skip = c->detect_len + (size_t)len - READ_SIZE;
            memmove(c->detect_buf, c->detect_buf + skip, READ_SIZE - skip);
            c->detect_len -= skip;
        }
        memcpy(c->detect_buf + c->detect_len, data, (size_t)len);
        c->detect_len += (size_t)len;
        memset(c->detect_buf + c->detect_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        enum AVCodecID cid = detect_codec(c->detect_buf, (int)c->detect_len);
        if (cid == AV_CODEC_ID_NONE) return;
        const AVCodec *codec = avcodec_find_decoder(cid);
        if (!codec) { obs_log(LOG_ERROR, "no decoder for codec %d", (int)cid); return; }
        c->ctx = avcodec_alloc_context3(codec);
        c->parser = av_parser_init(cid);
        if (!c->ctx || !c->parser) { obs_log(LOG_ERROR, "could not allocate decoder"); return; }
        c->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        c->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
        c->ctx->thread_count = 1;
        if (avcodec_open2(c->ctx, codec, NULL) < 0) { obs_log(LOG_ERROR, "could not open %s decoder",
                        cid == AV_CODEC_ID_HEVC ? "HEVC" : "H.264"); return; }
        c->codec_ready = true;
        obs_log(LOG_INFO, "udp stream codec: %s (%zu bytes buffered)",
                cid == AV_CODEC_ID_HEVC ? "HEVC" : "H.264", c->detect_len);
        uint8_t *d = c->detect_buf;
        int sz = (int)c->detect_len;
        c->detect_len = 0;
        memset(c->detect_buf, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        while (sz > 0) {
            int used = av_parser_parse2(c->parser, c->ctx, &c->pkt->data, &c->pkt->size, d, sz,
                                        AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) break;
            d += used; sz -= used;
            if (c->pkt->size > 0) { c->s->packets++; decode_packet(c->s, c->ctx, c->pkt, c->frame); }
        }
        c->codec_ready = true;
        return;
    }
    uint8_t *dp = (uint8_t *)data;
    int size = len;
    while (size > 0) {
        int used = av_parser_parse2(c->parser, c->ctx, &c->pkt->data, &c->pkt->size, dp, size,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) { c->s->decode_errors++; break; }
        dp += used;
        size -= used;
        if (c->pkt->size > 0) { c->s->packets++; decode_packet(c->s, c->ctx, c->pkt, c->frame); }
        if (used == 0) break;
    }
}

static void udp_flush(struct udp_ctx *c)
{
    while (1) {
        uint32_t s = c->next_seq;
        struct chunk *ch = &c->ring[s % RING_SIZE];
        if (ch->valid && ch->seq == s) {
            udp_feed(c, ch->data, ch->len);
            ch->valid = false;
            c->next_seq++;
            continue;
        }
        if (c->codec_ready) {
            uint32_t bid = s / FEC_BLOCK;
            int pos = (int)(s % FEC_BLOCK);
            if (pos < FEC_K) {
                struct block_slot *b = &c->blocks[bid % BLOCK_SLOTS];
                if (b->block_id == (int32_t)bid && b->pvalid && !b->dvalid[pos]) {
                    bool ok = true;
                    for (int p = 0; p < FEC_K; p++) {
                        if (p != pos && !b->dvalid[p]) { ok = false; break; }
                    }
                    if (ok) {
                        uint8_t out[UDP_MAX_PAYLOAD];
                        memcpy(out, b->parity, UDP_MAX_PAYLOAD);
                        for (int p = 0; p < FEC_K; p++) {
                            if (p != pos) {
                                for (int i = 0; i < UDP_MAX_PAYLOAD; i++) out[i] ^= b->data[p][i];
                            }
                        }
                        udp_feed(c, out, UDP_MAX_PAYLOAD);
                        b->dvalid[pos] = true;
                        c->next_seq++;
                        continue;
                    }
                }
            }
        }
        break;
    }
}

static void udp_check_stall(struct udp_ctx *c)
{
    uint32_t s = c->next_seq;
    struct chunk *ch = &c->ring[s % RING_SIZE];
    bool deliverable = (ch->valid && ch->seq == s);
    if (!deliverable && c->codec_ready) {
        uint32_t bid = s / FEC_BLOCK;
        int pos = (int)(s % FEC_BLOCK);
        if (pos < FEC_K) {
            struct block_slot *b = &c->blocks[bid % BLOCK_SLOTS];
            if (b->block_id == (int32_t)bid && b->pvalid && !b->dvalid[pos]) {
                bool ok = true;
                for (int p = 0; p < FEC_K; p++) {
                    if (p != pos && !b->dvalid[p]) { ok = false; break; }
                }
                if (ok) {
                    deliverable = true;
                }
            }
        }
    }
    if (!deliverable) {
        uint64_t now = os_gettime_ns();
        if (c->stall_ns == 0 || (now - c->stall_ns) > STALL_TIMEOUT_NS) {
            c->stall_ns = now;
            ch = &c->ring[s % RING_SIZE];
            ch->valid = false;
            uint32_t bid = s / FEC_BLOCK;
            int pos = (int)(s % FEC_BLOCK);
            if (pos < FEC_K) {
                struct block_slot *b = &c->blocks[bid % BLOCK_SLOTS];
                if (b->block_id == (int32_t)bid) b->dvalid[pos] = true;
            }
            c->next_seq++;
            c->stall_ns = 0;
        }
    } else {
        c->stall_ns = 0;
    }
}

static void run_session(struct lensgo_source *s, int fd, const char *host, int port)
{
    s->bytes = 0;
    s->packets = 0;
    s->frames = 0;
    s->decode_errors = 0;
    obs_log(LOG_INFO, "connected to %s:%d", host, port);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *buf = bmalloc(READ_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    AVCodecContext *ctx = NULL;
    AVCodecParserContext *parser = NULL;
    bool codec_ready = false;

    uint8_t *detect_buf = bmalloc(READ_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    size_t detect_len = 0;

    if (!pkt || !frame || !buf || !detect_buf) {
        obs_log(LOG_ERROR, "could not allocate decoder");
        goto cleanup;
    }

    while (!os_atomic_load_bool(&s->stop)) {
        if (os_atomic_load_long(&s->port) != (long)port) {
            obs_log(LOG_INFO, "port changed, reconnecting");
            break;
        }

        ssize_t n = recv(fd, buf, READ_SIZE, 0);
        if (n == 0) {
            obs_log(LOG_INFO, "phone closed the connection");
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            obs_log(LOG_WARNING, "receive error %d", errno);
            break;
        }

        s->bytes += (uint64_t)n;

        if (!codec_ready) {
            if (detect_len + (size_t)n > READ_SIZE) {
                size_t skip = detect_len + (size_t)n - READ_SIZE;
                memmove(detect_buf, detect_buf + skip, READ_SIZE - skip);
                detect_len -= skip;
            }
            memcpy(detect_buf + detect_len, buf, (size_t)n);
            detect_len += (size_t)n;
            memset(detect_buf + detect_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            enum AVCodecID codec_id = detect_codec(detect_buf, (int)detect_len);
            if (codec_id != AV_CODEC_ID_NONE) {
                const AVCodec *codec = avcodec_find_decoder(codec_id);
                if (!codec) {
                    obs_log(LOG_ERROR, "no decoder for codec %d", (int)codec_id);
                    break;
                }

                ctx = avcodec_alloc_context3(codec);
                parser = av_parser_init(codec_id);
                if (!ctx || !parser) {
                    obs_log(LOG_ERROR, "could not allocate decoder");
                    break;
                }

                ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
                ctx->flags2 |= AV_CODEC_FLAG2_FAST;
                ctx->thread_count = 1;

                if (avcodec_open2(ctx, codec, NULL) < 0) {
                    obs_log(LOG_ERROR, "could not open %s decoder",
                            codec_id == AV_CODEC_ID_HEVC ? "HEVC" : "H.264");
                    break;
                }

                codec_ready = true;
                obs_log(LOG_INFO, "stream codec: %s (%zu bytes buffered)",
                        codec_id == AV_CODEC_ID_HEVC ? "HEVC" : "H.264", detect_len);
            }
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
                s->packets++;
                decode_packet(s, ctx, pkt, frame);
            }
        }

        if (codec_ready && detect_len > 0) {
            uint8_t *data = detect_buf;
            int size = (int)detect_len;
            detect_len = 0;
            memset(data + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

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
    }

    obs_log(LOG_INFO, "session ended: %llu bytes, %llu packets, %llu frames, %llu decode errors",
            (unsigned long long)s->bytes, (unsigned long long)s->packets,
            (unsigned long long)s->frames, (unsigned long long)s->decode_errors);

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
    bfree(detect_buf);
}

static void run_udp_session(struct lensgo_source *s, int fd, const char *host, int port)
{
    s->bytes = 0;
    s->packets = 0;
    s->frames = 0;
    s->decode_errors = 0;
    obs_log(LOG_INFO, "connected to %s:%d (udp)", host, port);

    struct udp_ctx *c = bzalloc(sizeof(struct udp_ctx));
    c->s = s;
    c->ctx = NULL;
    c->parser = NULL;
    c->pkt = av_packet_alloc();
    c->frame = av_frame_alloc();
    c->detect_buf = bmalloc(READ_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
    c->detect_len = 0;
    c->codec_ready = false;
    c->next_seq = 0;
    memset(c->ring, 0, sizeof(c->ring));
    for (int i = 0; i < BLOCK_SLOTS; i++) c->blocks[i].block_id = -1;

    if (!c->pkt || !c->frame || !c->detect_buf) {
        obs_log(LOG_ERROR, "could not allocate udp session");
        goto cleanup;
    }

    uint8_t recv_buf[UDP_MAX_PAYLOAD + UDP_HEADER_SIZE];

    while (!os_atomic_load_bool(&s->stop)) {
        if (os_atomic_load_long(&s->port) != (long)port) {
            obs_log(LOG_INFO, "port changed, reconnecting");
            break;
        }

        ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
        if (n == 0) {
            obs_log(LOG_INFO, "phone closed the udp connection");
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                udp_check_stall(c);
                continue;
            }
            obs_log(LOG_WARNING, "receive error %d", errno);
            break;
        }
        if (n < (ssize_t)UDP_HEADER_SIZE) continue;

        s->bytes += (uint64_t)n;

        struct udp_header h;
        if (!udp_parse_header(recv_buf, (size_t)n, &h)) continue;
        if (h.magic != UDP_MAGIC) continue;
        if ((uint32_t)(h.seq - c->next_seq) >= RING_SIZE) continue;

        uint32_t idx = h.seq % RING_SIZE;
        struct chunk *ch = &c->ring[idx];
        if (ch->valid && ch->seq == h.seq) continue;
        memset(ch->data, 0, UDP_MAX_PAYLOAD);
        ch->valid = true;
        ch->seq = h.seq;
        ch->len = h.payload_len;
        memcpy(ch->data, recv_buf + UDP_HEADER_SIZE, h.payload_len);

        uint32_t bid = h.seq / FEC_BLOCK;
        int pos = (int)(h.seq % FEC_BLOCK);
        struct block_slot *b = &c->blocks[bid % BLOCK_SLOTS];
        if (b->block_id != (int32_t)bid) {
            memset(b, 0, sizeof(*b));
            b->block_id = (int32_t)bid;
        }
        b->mask |= (uint16_t)(1u << pos);
        if (pos < FEC_K) {
            memcpy(b->data[pos], ch->data, UDP_MAX_PAYLOAD);
            b->dvalid[pos] = true;
        } else {
            memcpy(b->parity, ch->data, UDP_MAX_PAYLOAD);
            b->pvalid = true;
        }

        udp_flush(c);
        c->stall_ns = 0;
    }

    obs_log(LOG_INFO, "udp session ended: %llu bytes, %llu packets, %llu frames, %llu decode errors",
            (unsigned long long)s->bytes, (unsigned long long)s->packets,
            (unsigned long long)s->frames, (unsigned long long)s->decode_errors);

cleanup:
    udp_free_ctx(c);
    obs_source_output_video(s->source, NULL);
}

static void *lensgo_thread(void *data)
{
    struct lensgo_source *s = data;

    while (!os_atomic_load_bool(&s->stop)) {
        int port = (int)os_atomic_load_long(&s->port);
        int mode = (int)os_atomic_load_long(&s->mode);

        char host[128];
        char adb_path[1024];
        pthread_mutex_lock(&s->lock);
        snprintf(host, sizeof(host), "%s", s->address);
        snprintf(adb_path, sizeof(adb_path), "%s", s->adb_path);
        pthread_mutex_unlock(&s->lock);

        int fd = -1;
        if (mode == CONNECTION_USB) {
            snprintf(host, sizeof(host), "%s", "127.0.0.1");
            uint64_t now = os_gettime_ns();
            if (s->last_forward_ns == 0 || now - s->last_forward_ns > FORWARD_RETRY_NS) {
                s->last_forward_ns = now;
                bool ok = run_adb_forward(adb_path, port);
                if (ok) {
                    if (!s->forward_ready_logged) {
                        obs_log(LOG_INFO, "adb forward ready for port %d", port);
                        s->forward_ready_logged = true;
                    }
                    s->forward_failed_logged = false;
                } else {
                    if (!s->forward_failed_logged) {
                        obs_log(LOG_WARNING, "auto adb forward failed: install platform-tools, "
                                             "connect the phone, and authorize USB debugging");
                        s->forward_failed_logged = true;
                    }
                    s->forward_ready_logged = false;
                }
            }
            fd = connect_to_address(host, port);
        } else if (mode == CONNECTION_WIFI) {
            if (host[0] == '\0') {
                if (!s->warned_no_address) {
                    obs_log(LOG_WARNING, "Wi-Fi mode is selected but no phone IP address is set");
                    s->warned_no_address = true;
                }
                os_sleep_ms(500);
                continue;
            }
            s->warned_no_address = false;
            fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (fd >= 0) {
                struct sockaddr_in addr;
                memset(&addr, 0, sizeof(addr));
                addr.sin_family = AF_INET;
                addr.sin_port = htons((uint16_t)port);
                if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
                    close(fd);
                    fd = -1;
                } else if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                    close(fd);
                    fd = -1;
                }
            }
            if (fd >= 0) {
                uint8_t hello[1] = {0};
                ssize_t hn = send(fd, hello, sizeof(hello), 0);
                if (hn < 0) { close(fd); fd = -1; }
            }
        } else {
            if (!s->warned_no_address) {
                obs_log(LOG_WARNING, "connection mode is unknown");
                s->warned_no_address = true;
            }
            os_sleep_ms(500);
            continue;
        }

        if (fd < 0) {
            uint64_t now = os_gettime_ns();
            if (s->last_connect_log_ns == 0 || now - s->last_connect_log_ns > 5000000000ULL) {
                s->last_connect_log_ns = now;
                obs_log(LOG_INFO, "waiting for phone at %s:%d (%s)", host, port,
                        mode == CONNECTION_USB ? "usb" : "wifi");
            }
            os_sleep_ms(500);
            continue;
        }
        s->last_connect_log_ns = 0;

        if (mode == CONNECTION_USB) {
            run_session(s, fd, host, port);
        } else {
            run_udp_session(s, fd, host, port);
        }
        if (mode == CONNECTION_USB) close(fd);
        os_sleep_ms(300);
    }
    return NULL;
}

static const char *lensgo_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "Lens Go";
}

static void lensgo_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, SETTING_PORT, DEFAULT_PORT);
    obs_data_set_default_int(settings, SETTING_MODE, CONNECTION_USB);
    obs_data_set_default_string(settings, SETTING_ADDRESS, "");
    obs_data_set_default_string(settings, SETTING_ADB, "");
    obs_data_set_default_int(settings, SETTING_LATENCY, LATENCY_LOWEST);
}

static bool lensgo_mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    UNUSED_PARAMETER(p);

    long mode = obs_data_get_int(settings, SETTING_MODE);
    bool usb = mode == CONNECTION_USB;

    obs_property_t *address = obs_properties_get(props, SETTING_ADDRESS);
    obs_property_t *adb = obs_properties_get(props, SETTING_ADB);
    if (address) {
        obs_property_set_visible(address, !usb);
    }
    if (adb) {
        obs_property_set_visible(adb, usb);
    }
    return false;
}

static obs_properties_t *lensgo_properties(void *data)
{
    struct lensgo_source *s = data;
    long current_mode = s ? (long)os_atomic_load_long(&s->mode) : CONNECTION_USB;

    obs_properties_t *props = obs_properties_create();

    obs_property_t *mode = obs_properties_add_list(props, SETTING_MODE, "Connection",
                                                   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(mode, "USB (automatic adb forward)", CONNECTION_USB);
    obs_property_list_add_int(mode, "Wi-Fi (phone IP address)", CONNECTION_WIFI);

    obs_property_t *address =
        obs_properties_add_text(props, SETTING_ADDRESS, "Phone IP address (Wi-Fi)",
                                OBS_TEXT_DEFAULT);
    obs_properties_add_int(props, SETTING_PORT, "Port", 1024, 65535, 1);
    obs_property_t *adb =
        obs_properties_add_text(props, SETTING_ADB, "adb path (optional)", OBS_TEXT_DEFAULT);

    bool usb = current_mode == CONNECTION_USB;
    if (address) {
        obs_property_set_visible(address, !usb);
    }
    if (adb) {
        obs_property_set_visible(adb, usb);
    }
    obs_property_set_modified_callback(mode, lensgo_mode_modified);

    obs_property_t *latency = obs_properties_add_list(props, SETTING_LATENCY, "Latency mode",
                                                      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(latency, "Lowest latency", LATENCY_LOWEST);
    obs_property_list_add_int(latency, "Smooth (adds a small delay)", LATENCY_SMOOTH);
    return props;
}

static void lensgo_update(void *data, obs_data_t *settings)
{
    struct lensgo_source *s = data;

    long port = (long)obs_data_get_int(settings, SETTING_PORT);
    if (port < 1024 || port > 65535) {
        port = DEFAULT_PORT;
    }
    if (port != os_atomic_load_long(&s->port)) {
        os_atomic_set_long(&s->port, port);
        s->last_forward_ns = 0;
        obs_log(LOG_INFO, "port set to %ld", port);
    }

    long mode = (long)obs_data_get_int(settings, SETTING_MODE);
    if (mode != CONNECTION_USB && mode != CONNECTION_WIFI) {
        mode = CONNECTION_USB;
    }
    if (mode != os_atomic_load_long(&s->mode)) {
        os_atomic_set_long(&s->mode, mode);
        s->last_forward_ns = 0;
        obs_log(LOG_INFO, "connection mode: %s", mode == CONNECTION_USB ? "USB" : "Wi-Fi");
    }

    const char *address = obs_data_get_string(settings, SETTING_ADDRESS);
    const char *adb_path = obs_data_get_string(settings, SETTING_ADB);

    pthread_mutex_lock(&s->lock);
    snprintf(s->address, sizeof(s->address), "%s", address ? address : "");
    snprintf(s->adb_path, sizeof(s->adb_path), "%s", adb_path ? adb_path : "");
    pthread_mutex_unlock(&s->lock);

    bool lowest = obs_data_get_int(settings, SETTING_LATENCY) == LATENCY_LOWEST;
    obs_source_set_async_unbuffered(s->source, lowest);
}

static void *lensgo_create(obs_data_t *settings, obs_source_t *source)
{
    struct lensgo_source *s = bzalloc(sizeof(struct lensgo_source));
    s->source = source;
    os_atomic_set_bool(&s->stop, false);
    os_atomic_set_long(&s->port, DEFAULT_PORT);
    os_atomic_set_long(&s->mode, CONNECTION_USB);
    pthread_mutex_init(&s->lock, NULL);
    lensgo_update(s, settings);

    if (pthread_create(&s->thread, NULL, lensgo_thread, s) == 0) {
        s->thread_started = true;
        obs_log(LOG_INFO, "source created, waiting for the phone");
    } else {
        obs_log(LOG_ERROR, "could not start the receiver thread");
    }
    return s;
}

static void lensgo_destroy(void *data)
{
    struct lensgo_source *s = data;

    os_atomic_set_bool(&s->stop, true);
    if (s->thread_started) {
        pthread_join(s->thread, NULL);
    }
    pthread_mutex_destroy(&s->lock);
    bfree(s);
}

static struct obs_source_info lensgo_info = {
    .id = "lensgo_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .icon_type = OBS_ICON_TYPE_CAMERA,
    .get_name = lensgo_get_name,
    .get_defaults = lensgo_defaults,
    .get_properties = lensgo_properties,
    .update = lensgo_update,
    .create = lensgo_create,
    .destroy = lensgo_destroy,
};

bool obs_module_load(void)
{
    unsigned int v = avcodec_version();
    obs_register_source(&lensgo_info);
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    obs_log(LOG_INFO, "libavcodec %u.%u.%u", v >> 16, (v >> 8) & 0xff, v & 0xff);
    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "plugin unloaded");
}
