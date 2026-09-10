/*
 * Copyright (c) 2026 Bevara
 * This file is part of GPAC / FLV demuxer filter
 * Licensed under GNU LGPL v3
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/list.h>
#include <gpac/bitstream.h>
#include <flv.h>

typedef struct {
    GF_FilterPid *opid;
    Bool in_use;
    Bool done;
    u64 ts;
} FLVStream;

typedef struct {
    GF_FilterPid *ipid;
    GF_FilterPid *v_opid, *a_opid;
    const char *src_url;
    u64 file_size;
    
    FLVContext *flv_ctx;
    Bool video_config_ready, audio_config_ready;
    
    u32 audio_profile, audio_sample_rate, audio_channels;
    
    FLVStream video_stream, audio_stream;
    Bool video_done, audio_done;
    Bool opened;
    u32 v_frame_count, a_frame_count;
} GF_FLVDMXCtx;

static void flvdmx_video_callback(enum FLVVideoType video_type, int64_t timestamp, uint8_t *data, uint32_t data_len, void *arg)
{
    GF_FLVDMXCtx *ctx = (GF_FLVDMXCtx *)arg;
    if (!ctx->v_opid || ctx->video_done || !data) return;
    
    u8 *video_buf = gf_malloc(data_len);
    if (!video_buf) return;
    memcpy(video_buf, data, data_len);
    
    GF_FilterPacket *pck = gf_filter_pck_new_alloc(ctx->v_opid, data_len, &video_buf);
    if (!pck) {
        gf_free(video_buf);
        return;
    }
    
    gf_filter_pck_set_cts(pck, (double)timestamp / 1000.0);
    gf_filter_pck_set_duration(pck, 40);
    
    if (data_len > 0 && (data[0] & 0x0f) == 0x05) {
        gf_filter_pck_set_sap(pck, GF_FILTER_SAP_1);
    } else {
        gf_filter_pck_set_sap(pck, 0);
    }
    
    gf_filter_pck_set_framing(pck, GF_TRUE, GF_TRUE);
    gf_filter_pck_send(pck);
    
    ctx->video_stream.ts = timestamp;
    ctx->v_frame_count++;
}

static void flvdmx_audio_callback(enum FLVAudioType audio_type, int profile, int sample_rate_index, int channel, int64_t timestamp, uint8_t *data, uint32_t data_len, void *arg)
{
    GF_FLVDMXCtx *ctx = (GF_FLVDMXCtx *)arg;
    if (!ctx->a_opid || ctx->audio_done) return;
    
    if (!ctx->audio_config_ready && data) {
        ctx->audio_profile = profile;
        ctx->audio_sample_rate = sample_rate_index;
        ctx->audio_channels = channel;
        ctx->audio_config_ready = GF_TRUE;
    }
    
    if (!data || !data_len) return;
    
    u8 *audio_buf = gf_malloc(data_len);
    if (!audio_buf) return;
    memcpy(audio_buf, data, data_len);
    
    GF_FilterPacket *pck = gf_filter_pck_new_alloc(ctx->a_opid, data_len, &audio_buf);
    if (!pck) {
        gf_free(audio_buf);
        return;
    }
    
    gf_filter_pck_set_cts(pck, (double)timestamp / 1000.0);
    gf_filter_pck_set_duration(pck, 40);
    gf_filter_pck_set_sap(pck, 0);
    gf_filter_pck_set_framing(pck, GF_TRUE, GF_TRUE);
    
    gf_filter_pck_send(pck);
    
    ctx->audio_stream.ts = timestamp;
    ctx->a_frame_count++;
}

static void flvdmx_script_callback(AMFDict dict, void *arg)
{
    (void)dict;
    (void)arg;
}

static GF_Err flvdmx_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool configured)
{
    GF_FLVDMXCtx *ctx = gf_filter_get_udta(filter);
    if (!configured) return GF_OK;
    
    if (!ctx->ipid) {
        ctx->ipid = pid;
    }
    
    return GF_OK;
}

static GF_Err flvdmx_process(GF_Filter *filter)
{
    GF_FLVDMXCtx *ctx = gf_filter_get_udta(filter);
    
    if (!ctx->ipid) {
        return GF_OK;
    }
    
    if (!ctx->opened) {
        const GF_PropertyValue *p;
        p = gf_filter_pid_get_property(ctx->ipid, GF_PROP_PID_FILEPATH);
        if (!p) return GF_OK;
        
        ctx->src_url = p->value.string;
        if (!ctx->src_url || !ctx->src_url[0]) return GF_OK;
        
        ctx->flv_ctx = createFLVContext();
        if (!ctx->flv_ctx) return GF_NOT_FOUND;
        
        setReadCallBack(ctx->flv_ctx, flvdmx_audio_callback, flvdmx_video_callback, flvdmx_script_callback, ctx);
        
        if (demuxerFLVFile(ctx->flv_ctx, (char *)ctx->src_url) != 0) {
            destroyFLVContext(ctx->flv_ctx);
            ctx->flv_ctx = NULL;
            return GF_NOT_FOUND;
        }
        
        ctx->opened = GF_TRUE;
    }
    
    if (ctx->video_done && ctx->audio_done) {
        return GF_EOS;
    }
    
    gf_filter_post_process_task(filter);
    return GF_OK;
}

static Bool flvdmx_process_event(GF_Filter *filter, const GF_FilterEvent *evt)
{
    GF_FLVDMXCtx *ctx = gf_filter_get_udta(filter);
    
    if (evt->type == GF_FEVT_PLAY_HINT) {
        if (evt->play_hint.full_file_only == GF_TRUE) {
            return GF_TRUE;
        }
    }
    
    return GF_TRUE;
}

static const char * flvdmx_probe_data(const u8 *data, u32 size, GF_FilterProbeScore *score)
{
    if (size < 9) return NULL;
    *score = GF_FPROBE_NOT_SUPPORTED;
    if (memcmp(data, "FLV", 3)) return NULL;
    if (data[4] != 0x01) return NULL;
    *score = GF_FPROBE_SUPPORTED;
    return "video/x-flv";
}

static const GF_FilterCapability FLVDMXCaps[] = {
    CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
    CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "flv"),
    CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "video/x-flv|video/flv"),
    CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
    CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
};

#define OFFS(_n)    #_n, offsetof(GF_FLVDMXCtx, _n)
static const GF_FilterArgs FLVDMXArgs[] = {
    {0}
};

GF_FilterRegister FLVDMXRegister = {
    .name = "flvdmx",
    GF_FS_SET_DESCRIPTION("FLV demultiplexer")
    GF_FS_SET_HELP("This filter demultiplexes FLV (Flash Video) files to produce H.264/H.265 video and AAC/MP3 audio PIDs.")
    .private_size = sizeof(GF_FLVDMXCtx),
    .flags = GF_FS_REG_USE_SYNC_READ,
    .initialize = NULL,
    .finalize = NULL,
    .args = FLVDMXArgs,
    SETCAPS(FLVDMXCaps),
    .configure_pid = flvdmx_configure_pid,
    .process = flvdmx_process,
    .process_event = flvdmx_process_event,
    .probe_data = flvdmx_probe_data,
    .hint_class_type = GF_FS_CLASS_DEMULTIPLEXER
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE flvdmx_register(GF_FilterSession *session)
{
    return &FLVDMXRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_flvdmx(void) {
    gf_filter_auto_register("flvdmx", flvdmx_register);
}
