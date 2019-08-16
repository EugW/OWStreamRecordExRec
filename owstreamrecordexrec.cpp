#include <Windows.h>
#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("OWStreamRecordExRec", "en-US")
#define do_log(level, format, ...) \
	blog(level, "[OWStreamRecordExRec-filter] " format, ##__VA_ARGS__)

#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)

#define SETTING_INTERVAL "interval"

struct OWStreamRecordExRec_data {
    obs_source_t *context;
    double interval;
    float since_last;
    bool capture;
    uint32_t width;
    uint32_t height;
    gs_texrender_t *texrender;
    gs_stagesurf_t *staging_texture;
    uint8_t *data;
    uint32_t linesize;
    uint32_t index;
    uint32_t shmem_size;
    HANDLE shmem;
    HANDLE mutex;
};

const char *shmem_name = "OWStreamRecordExRec:SHMEM";

void UploadThread(struct OWStreamRecordExRec_data *filter) {
    if (filter->shmem) {
        auto *buf = (uint32_t *) MapViewOfFile(
                filter->shmem,
                FILE_MAP_ALL_ACCESS,
                0,
                0,
                filter->shmem_size
        );
        if (buf) {
            buf[0] = filter->width;
            buf[1] = filter->height;
            buf[2] = filter->linesize;
            buf[3] = filter->index;
            memcpy(&buf[4], filter->data, filter->linesize * filter->height);
        }
        UnmapViewOfFile(buf);
    }
}

void *OWStreamRecordExRec_create(obs_data_t *settings, obs_source_t *context) {
    auto *filter = reinterpret_cast<OWStreamRecordExRec_data *>(bzalloc(sizeof(struct OWStreamRecordExRec_data)));
    filter->context = context;
    obs_enter_graphics();
    filter->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    obs_leave_graphics();
    filter->interval = 0.5;
    filter->mutex = CreateMutexA(nullptr, FALSE, nullptr);
    obs_source_update(context, settings);
    return filter;
}

void OWStreamRecordExRec_destroy(struct OWStreamRecordExRec_data *filter) {
    WaitForSingleObject(filter->mutex, INFINITE);
    obs_enter_graphics();
    gs_texrender_destroy(filter->texrender);
    if (filter->staging_texture) {
        gs_stagesurface_destroy((gs_stagesurf_t *) filter->staging_texture);
    }
    obs_leave_graphics();
    if (filter->data) {
        bfree(filter->data);
    }
    if (filter->shmem) {
        CloseHandle(filter->shmem);
    }
    ReleaseMutex(filter->mutex);
    CloseHandle(filter->mutex);
    bfree(filter);
}

void OWStreamRecordExRec_tick(struct OWStreamRecordExRec_data *filter, float t) {
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target) {
        filter->width = 0;
        filter->height = 0;
        if (filter->staging_texture) {
            obs_enter_graphics();
            gs_stagesurface_destroy(filter->staging_texture);
            obs_leave_graphics();
            filter->staging_texture = nullptr;
        }
        if (filter->data) {
            bfree(filter->data);
            filter->data = nullptr;
        }
        return;
    }
    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);
    if (width == 0 && height == 0)
        return;
    WaitForSingleObject(filter->mutex, INFINITE);
    bool update = false;
    if (width != filter->width || height != filter->height) {
        update = true;
        filter->width = width;
        filter->height = height;
        obs_enter_graphics();
        if (filter->staging_texture) {
            gs_stagesurface_destroy(filter->staging_texture);
        }
        filter->staging_texture = gs_stagesurface_create(filter->width, filter->height, GS_RGBA);
        obs_leave_graphics();
        info("Created Staging texture %d by %d: %x", width, height, filter->staging_texture);
        if (filter->data) {
            bfree(filter->data);
        }
        filter->data = reinterpret_cast<uint8_t *>(bzalloc((width + 32) * height * 4));
        filter->capture = false;
        filter->since_last = 0.0f;
    }
    if (shmem_name) {
        if (update) {
            if (filter->shmem) {
                info("Closing shmem \"%s\": %x", shmem_name, filter->shmem);
                CloseHandle(filter->shmem);
            }
            filter->shmem_size = 12 + (width + 32) * height * 4;
            filter->shmem = CreateFileMapping(
                    INVALID_HANDLE_VALUE,
                    nullptr,
                    PAGE_READWRITE,
                    0,
                    filter->shmem_size,
                    shmem_name
            );
            info("Created shmem \"%s\": %x", shmem_name, filter->shmem);
        }
    }
    filter->since_last += t;
    if (filter->since_last > filter->interval - 0.05) {
        filter->capture = true;
        filter->since_last = 0.0f;
    }
    ReleaseMutex(filter->mutex);
}

void OWStreamRecordExRec_render(struct OWStreamRecordExRec_data *filter, gs_effect_t *effect) {
    UNUSED_PARAMETER(effect);
    obs_source_t *target = obs_filter_get_target(filter->context);
    obs_source_t *parent = obs_filter_get_parent(filter->context);
    if (!parent || !filter->width || !filter->height || !filter->capture) {
        obs_source_skip_video_filter(filter->context);
        return;
    }
    gs_texrender_reset(filter->texrender);
    gs_blend_state_push();
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
    if (gs_texrender_begin(filter->texrender, filter->width, filter->height)) {
        uint32_t parent_flags = obs_source_get_output_flags(target);
        bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
        bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
        struct vec4 clear_color{};
        vec4_zero(&clear_color);
        gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
        gs_ortho(0.0f, (float) filter->width, 0.0f, (float) filter->height, -100.0f, 100.0f);
        if (target == parent && !custom_draw && !async) obs_source_default_render(target);
        else obs_source_video_render(target);
        gs_texrender_end(filter->texrender);
    }
    gs_blend_state_pop();
    gs_effect_t *effect2 = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_texture_t *tex = gs_texrender_get_texture(filter->texrender);
    if (tex) {
        gs_stage_texture(filter->staging_texture, tex);
        uint32_t linesize;
        WaitForSingleObject(filter->mutex, INFINITE);
        if (gs_stagesurface_map(filter->staging_texture, &filter->data, &linesize)) {
            filter->linesize = linesize;
            CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(UploadThread), filter, 0, nullptr);
            gs_stagesurface_unmap(filter->staging_texture);
        }
        filter->capture = false;
        ReleaseMutex(filter->mutex);
        gs_eparam_t *image = gs_effect_get_param_by_name(effect2, "image");
        gs_effect_set_texture(image, tex);
        while (gs_effect_loop(effect2, "Draw"))
            gs_draw_sprite(tex, 0, filter->width, filter->height);
    }
}

bool obs_module_load(void) {
    obs_source_info OWStreamRecordExRec = {};
    OWStreamRecordExRec.id = "OWStreamRecordExRec";
    OWStreamRecordExRec.type = OBS_SOURCE_TYPE_FILTER;
    OWStreamRecordExRec.output_flags = OBS_SOURCE_VIDEO;
    OWStreamRecordExRec.get_name = [](void *) { return obs_module_text("OWStreamRecordExRec"); };
    OWStreamRecordExRec.get_properties = [](void *) {
        obs_properties_t *props = obs_properties_create();
        obs_properties_add_float_slider(props, SETTING_INTERVAL, "Interval (seconds)", 0.01, 1, 0.01);
        return props;
    };
    OWStreamRecordExRec.get_defaults = [](obs_data_t *settings) {
        obs_data_set_default_double(settings, SETTING_INTERVAL, 0.25);
    };
    OWStreamRecordExRec.update = [](void *data, obs_data_t *settings) {
        auto *filter = reinterpret_cast<OWStreamRecordExRec_data *>(data);
        WaitForSingleObject(filter->mutex, INFINITE);
        filter->interval = obs_data_get_double(settings, SETTING_INTERVAL);
        ReleaseMutex(filter->mutex);
    };
    OWStreamRecordExRec.create = OWStreamRecordExRec_create;
    OWStreamRecordExRec.destroy = [](void *data) {
        OWStreamRecordExRec_destroy(reinterpret_cast<OWStreamRecordExRec_data *>(data));
    };
    OWStreamRecordExRec.video_tick = [](void *data, float t) {
        OWStreamRecordExRec_tick(reinterpret_cast<OWStreamRecordExRec_data *>(data), t);
    };
    OWStreamRecordExRec.video_render = [](void *data, gs_effect_t *effect) {
        OWStreamRecordExRec_render(reinterpret_cast<OWStreamRecordExRec_data *>(data), effect);
    };
    obs_register_source(&OWStreamRecordExRec);
    return true;
}