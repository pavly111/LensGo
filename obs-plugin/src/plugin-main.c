#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <plugin-support.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

#define TEST_WIDTH 1280
#define TEST_HEIGHT 720
#define TEST_FPS 30

struct phonecam_source {
    obs_source_t *source;
    pthread_t thread;
    bool thread_started;
    volatile bool stop;
    uint8_t *buffer;
};

static void draw_test_frame(uint8_t *buf, uint32_t w, uint32_t h, uint32_t tick)
{
    static const uint32_t colors[8] = {
        0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00,
        0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000,
    };
    uint32_t *px = (uint32_t *)buf;
    uint32_t marker = (tick * 10) % w;

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = colors[(x * 8) / w];
            if (y >= (h * 3) / 4) {
                uint32_t d = (x + w - marker) % w;
                c = (d < 80) ? 0xFFFFFFFF : 0xFF202020;
            }
            px[y * w + x] = c;
        }
    }
}

static void *phonecam_thread(void *data)
{
    struct phonecam_source *s = data;
    uint32_t tick = 0;

    while (!os_atomic_load_bool(&s->stop)) {
        draw_test_frame(s->buffer, TEST_WIDTH, TEST_HEIGHT, tick++);

        struct obs_source_frame frame;
        memset(&frame, 0, sizeof(frame));
        frame.width = TEST_WIDTH;
        frame.height = TEST_HEIGHT;
        frame.format = VIDEO_FORMAT_BGRA;
        frame.linesize[0] = TEST_WIDTH * 4;
        frame.data[0] = s->buffer;
        frame.timestamp = os_gettime_ns();
        obs_source_output_video(s->source, &frame);

        os_sleep_ms(1000 / TEST_FPS);
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
    s->buffer = bmalloc(TEST_WIDTH * TEST_HEIGHT * 4);
    os_atomic_set_bool(&s->stop, false);

    if (pthread_create(&s->thread, NULL, phonecam_thread, s) == 0) {
        s->thread_started = true;
    } else {
        obs_log(LOG_ERROR, "could not start the video thread");
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
    bfree(s->buffer);
    bfree(s);
}

static struct obs_source_info phonecam_info = {
    .id = "openphonecam_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO,
    .icon_type = OBS_ICON_TYPE_CAMERA,
    .get_name = phonecam_get_name,
    .create = phonecam_create,
    .destroy = phonecam_destroy,
};

bool obs_module_load(void)
{
    obs_register_source(&phonecam_info);
    obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    obs_log(LOG_INFO, "plugin unloaded");
}
