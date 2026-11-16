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

struct phonecam_source {
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

    if (s->frames == 0) {
        obs_log(LOG_INFO, "first frame received: %dx%d", f->width, f->height);
    }
    s->frames++;
}

static void decode_packet(struct phonecam_source *s, AVCodecContext *ctx, AVPacket *pkt, AVFrame *frame)
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

static void run_session(struct phonecam_source *s, int fd, const char *host, int port)
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

    if (!pkt || !frame || !buf) {
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

        if (s->bytes == 0) {
            obs_log(LOG_INFO, "first data received: %zd bytes", n);
        }
        s->bytes += (uint64_t)n;

        if (!codec_ready) {
            enum AVCodecID codec_id = detect_codec(buf, (int)n);
            if (codec_id == AV_CODEC_ID_NONE) {
                codec_id = AV_CODEC_ID_H264;
                obs_log(LOG_INFO, "codec not detected yet, assuming H.264");
            }

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
            obs_log(LOG_INFO, "stream codec: %s",
                    codec_id == AV_CODEC_ID_HEVC ? "HEVC" : "H.264");
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
                if (s->packets == 0) {
                    obs_log(LOG_INFO, "first packet parsed: %d bytes", pkt->size);
                }
                s->packets++;
                decode_packet(s, ctx, pkt, frame);
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
}

static void *phonecam_thread(void *data)
{
    struct phonecam_source *s = data;

    while (!os_atomic_load_bool(&s->stop)) {
        int port = (int)os_atomic_load_long(&s->port);
        int mode = (int)os_atomic_load_long(&s->mode);

        char host[128];
        char adb_path[1024];
        pthread_mutex_lock(&s->lock);
        snprintf(host, sizeof(host), "%s", s->address);
        snprintf(adb_path, sizeof(adb_path), "%s", s->adb_path);
        pthread_mutex_unlock(&s->lock);

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
        } else if (host[0] == '\0') {
            if (!s->warned_no_address) {
                obs_log(LOG_WARNING, "Wi-Fi mode is selected but no phone IP address is set");
                s->warned_no_address = true;
            }
            os_sleep_ms(500);
            continue;
        } else {
            s->warned_no_address = false;
        }

        int fd = connect_to_address(host, port);
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

        run_session(s, fd, host, port);
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

static void phonecam_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, SETTING_PORT, DEFAULT_PORT);
    obs_data_set_default_int(settings, SETTING_MODE, CONNECTION_USB);
    obs_data_set_default_string(settings, SETTING_ADDRESS, "");
    obs_data_set_default_string(settings, SETTING_ADB, "");
    obs_data_set_default_int(settings, SETTING_LATENCY, LATENCY_LOWEST);
}

static bool phonecam_mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
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

static obs_properties_t *phonecam_properties(void *data)
{
    struct phonecam_source *s = data;
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
    obs_property_set_modified_callback(mode, phonecam_mode_modified);

    obs_property_t *latency = obs_properties_add_list(props, SETTING_LATENCY, "Latency mode",
                                                      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(latency, "Lowest latency", LATENCY_LOWEST);
    obs_property_list_add_int(latency, "Smooth (adds a small delay)", LATENCY_SMOOTH);
    return props;
}

static void phonecam_update(void *data, obs_data_t *settings)
{
    struct phonecam_source *s = data;

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

static void *phonecam_create(obs_data_t *settings, obs_source_t *source)
{
    struct phonecam_source *s = bzalloc(sizeof(struct phonecam_source));
    s->source = source;
    os_atomic_set_bool(&s->stop, false);
    os_atomic_set_long(&s->port, DEFAULT_PORT);
    os_atomic_set_long(&s->mode, CONNECTION_USB);
    pthread_mutex_init(&s->lock, NULL);
    phonecam_update(s, settings);

    if (pthread_create(&s->thread, NULL, phonecam_thread, s) == 0) {
        s->thread_started = true;
        obs_log(LOG_INFO, "source created, waiting for the phone");
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
    pthread_mutex_destroy(&s->lock);
    bfree(s);
}

static struct obs_source_info phonecam_info = {
    .id = "openphonecam_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .icon_type = OBS_ICON_TYPE_CAMERA,
    .get_name = phonecam_get_name,
    .get_defaults = phonecam_defaults,
    .get_properties = phonecam_properties,
    .update = phonecam_update,
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
