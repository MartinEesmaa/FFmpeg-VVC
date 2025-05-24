/*
 * WebRTC-HTTP egress protocol (WHEP) demuxer using libdatachannel
 *
 * Copyright (C) 2023 NativeWaves GmbH <contact@nativewaves.com>
 * This work is supported by FFG project 47168763.
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/time.h"
#include "libavcodec/packet_internal.h"
#include "libavutil/random_seed.h"
#include "version.h"
#include "rtsp.h"
#include "webrtc.h"

typedef struct WHEPContext {
    const AVClass *av_class;
    WebRTCContext webrtc_ctx;
    int reordering_queue_size;
    PacketList queue;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} WHEPContext;

static void* ff_webrtc_read_thread(void* ptr)
{
    WebRTCTrack*const track = (WebRTCTrack*const)ptr;
    WHEPContext*const ctx = (WHEPContext*const)track->avctx->priv_data;
    int ret;

    while (!rtcIsClosed(track->track_id)) {
        AVPacket pkt = { 0 };
        ret = av_read_frame(track->rtp_ctx, &pkt);
        if (ret < 0) {
            if (!rtcIsClosed(track->track_id))
                av_log(track->avctx, AV_LOG_ERROR, "av_read_frame failed: %s\n", av_err2str(ret));
            av_packet_unref(&pkt);
            break;
        }

        pkt.stream_index = track->stream_index;

        pthread_mutex_lock(&ctx->mutex);
        ret = avpriv_packet_list_put(&ctx->queue, &pkt, NULL, 0);
        if (ret == 0)
        {
            pthread_cond_signal(&ctx->cond);
        }
        pthread_mutex_unlock(&ctx->mutex);
        if (ret < 0) {
            av_log(track->avctx, AV_LOG_ERROR, "avpriv_packet_list_put failed: %s\n", av_err2str(ret));
            av_packet_unref(&pkt);
            break;
        }
    }

    rtcClose(track->track_id);
    pthread_cond_signal(&ctx->cond);
    return NULL;
}

static int whep_read_header(AVFormatContext* avctx)
{
    WHEPContext*const ctx = (WHEPContext*const)avctx->priv_data;
    int ret, i;
    char media_stream_id[37] = { 0 };
    rtcTrackInit track_init;
    AVDictionary* options = NULL;
    const AVInputFormat* infmt;
    AVStream* stream;
    FFIOContext sdp_pb;
    int64_t timeout;

    ff_webrtc_init_logger();
    ret = ff_webrtc_init_connection(&ctx->webrtc_ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize connection\n");
        goto fail;
    }

    /* configure audio and video track */
    ret = ff_webrtc_generate_media_stream_id(media_stream_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        goto fail;
    }
    ctx->webrtc_ctx.tracks = av_mallocz(2 * sizeof(WebRTCTrack));
    ctx->webrtc_ctx.nb_tracks = 2;
    ctx->webrtc_ctx.avctx = avctx;
    if (!ctx->webrtc_ctx.tracks) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    for (i=0; i < ctx->webrtc_ctx.nb_tracks; i++) {
        ctx->webrtc_ctx.tracks[i].avctx = avctx;
    }

    /* configure video track */
    memset(&track_init, 0, sizeof(rtcTrackInit));
    track_init.direction = RTC_DIRECTION_RECVONLY;
    track_init.codec = RTC_CODEC_H264; // TODO: support more codecs once libdatachannel C api supports them
    track_init.payloadType = 96;
    track_init.ssrc = av_get_random_seed();
    track_init.mid = "0";
    track_init.name = LIBAVFORMAT_IDENT;
    track_init.msid = media_stream_id;
    track_init.trackId = av_asprintf("%s-video", media_stream_id);
    track_init.profile = "profile-level-id=42e01f;packetization-mode=1;level-asymmetry-allowed=1";

    ctx->webrtc_ctx.tracks[0].track_id = rtcAddTrackEx(ctx->webrtc_ctx.peer_connection, &track_init);
    if (!ctx->webrtc_ctx.tracks[0].track_id) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    if (track_init.trackId)
        av_freep(&track_init.trackId);

    /* configure audio track */
    memset(&track_init, 0, sizeof(rtcTrackInit));
    track_init.direction = RTC_DIRECTION_RECVONLY;
    track_init.codec = RTC_CODEC_OPUS; // TODO: support more codecs once libdatachannel C api supports them
    track_init.payloadType = 97;
    track_init.ssrc = av_get_random_seed();
    track_init.mid = "1";
    track_init.name = LIBAVFORMAT_IDENT;
    track_init.msid = media_stream_id;
    track_init.trackId = av_asprintf("%s-audio", media_stream_id);
    track_init.profile = "minptime=10;maxaveragebitrate=96000;stereo=1;sprop-stereo=1;useinbandfec=1";

    ctx->webrtc_ctx.tracks[1].track_id = rtcAddTrackEx(ctx->webrtc_ctx.peer_connection, &track_init);
    if (!ctx->webrtc_ctx.tracks[1].track_id) {
        av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
        ret = AVERROR_EXTERNAL;
        goto fail;
    }
    if (track_init.trackId)
        av_freep(&track_init.trackId);

    /* create resource */
    ret = ff_webrtc_create_resource(&ctx->webrtc_ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "webrtc_create_resource failed\n");
        goto fail;
    }

    /* wait for connection to be established */
    timeout = av_gettime_relative() + ctx->webrtc_ctx.connection_timeout;
    while (ctx->webrtc_ctx.state != RTC_CONNECTED) {
        if (ctx->webrtc_ctx.state == RTC_FAILED || ctx->webrtc_ctx.state == RTC_CLOSED || av_gettime_relative() > timeout) {
            av_log(avctx, AV_LOG_ERROR, "Failed to open connection\n");
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        av_log(avctx, AV_LOG_VERBOSE, "Waiting for PeerConnection to open\n");
        av_usleep(1000);
    }

    /* initialize SDP muxer per track */
    for (int i = 0; i < ctx->webrtc_ctx.nb_tracks; i++) {
        char sdp_track[SDP_MAX_SIZE] = { 0 };
        ret = rtcGetTrackDescription(ctx->webrtc_ctx.tracks[i].track_id, sdp_track, sizeof(sdp_track));
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "rtcGetTrackDescription failed\n");
            goto fail;
        }

        ffio_init_read_context(&sdp_pb, (uint8_t*)sdp_track, strlen(sdp_track));

        infmt = av_find_input_format("sdp");
        if (!infmt)
            goto fail;
        ctx->webrtc_ctx.tracks[i].rtp_ctx = avformat_alloc_context();
        if (!ctx->webrtc_ctx.tracks[i].rtp_ctx) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->webrtc_ctx.tracks[i].rtp_ctx->max_delay = avctx->max_delay;
        ctx->webrtc_ctx.tracks[i].rtp_ctx->pb = &sdp_pb.pub;
        ctx->webrtc_ctx.tracks[i].rtp_ctx->interrupt_callback = avctx->interrupt_callback;

        if ((ret = ff_copy_whiteblacklists(ctx->webrtc_ctx.tracks[i].rtp_ctx, avctx)) < 0)
            goto fail;

        av_dict_set(&options, "sdp_flags", "custom_io", 0);
        av_dict_set_int(&options, "reorder_queue_size", ctx->reordering_queue_size, 0);

        ret = avformat_open_input(&ctx->webrtc_ctx.tracks[i].rtp_ctx, "temp.sdp", infmt, &options);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "avformat_open_input failed\n");
            goto fail;
        }

        ret = ff_webrtc_init_urlcontext(&ctx->webrtc_ctx, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "webrtc_init_urlcontext failed\n");
            goto fail;
        }
        ret = ffio_fdopen(&ctx->webrtc_ctx.tracks[i].rtp_ctx->pb, ctx->webrtc_ctx.tracks[i].rtp_url_context);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ffio_fdopen failed\n");
            goto fail;
        }

        /* copy codec parameters */
        stream = avformat_new_stream(avctx, NULL);
        if (!stream) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ctx->webrtc_ctx.tracks[i].stream_index = stream->index;

        ret = avcodec_parameters_copy(stream->codecpar, ctx->webrtc_ctx.tracks[i].rtp_ctx->streams[0]->codecpar);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "avcodec_parameters_copy failed\n");
            goto fail;
        }
        stream->time_base = ctx->webrtc_ctx.tracks[i].rtp_ctx->streams[0]->time_base;

        /* start reading thread */
        pthread_mutex_init(&ctx->mutex, NULL);
        pthread_cond_init(&ctx->cond, NULL);
        ret = pthread_create(&ctx->webrtc_ctx.tracks[i].thread, NULL, ff_webrtc_read_thread, &ctx->webrtc_ctx.tracks[i]);
        if (ret < 0) {
            ret = AVERROR_EXTERNAL;
            av_log(avctx, AV_LOG_ERROR, "pthread_create failed\n");
            goto fail;
        }
    }

    return 0;

fail:
    ff_webrtc_deinit(&ctx->webrtc_ctx, false);
    return ret;
}

static int whep_read_close(AVFormatContext* avctx)
{
    WHEPContext*const ctx = (WHEPContext*const)avctx->priv_data;
    int ret = 0;

    av_log(avctx, AV_LOG_VERBOSE, "Closing connection\n");

    /* close resource */
    ret = ff_webrtc_close_resource(&ctx->webrtc_ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "webrtc_close_resource failed\n");
    }

    ff_webrtc_deinit(&ctx->webrtc_ctx, false);

    /* free packet queue */
    avpriv_packet_list_free(&ctx->queue);
    pthread_mutex_destroy(&ctx->mutex);
    pthread_cond_destroy(&ctx->cond);

    return ret;
}

static int whep_read_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    WHEPContext*const ctx = (WHEPContext*const)avctx->priv_data;
    WebRTCTrack*const track = &ctx->webrtc_ctx.tracks[pkt->stream_index];
    int ret;

    if (avctx->pb && avctx->pb->error)
        return avctx->pb->error;

    pthread_mutex_lock(&ctx->mutex);
    for (;;) {
        ret = avpriv_packet_list_get(&ctx->queue, pkt);
        if (ret == 0) {
            break;
        }
        if (rtcIsClosed(track->track_id) || ctx->webrtc_ctx.state == RTC_CLOSED) {
            ret = AVERROR_EOF;
            break;
        }
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    pthread_mutex_unlock(&ctx->mutex);
    return ret;
}


#define OFFSET(x) offsetof(WHEPContext, x)
#define FLAGS AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    FF_WEBRTC_COMMON_OPTIONS,
    { "reorder_queue_size", "set number of packets to buffer for handling of reordered packets", OFFSET(reordering_queue_size), AV_OPT_TYPE_INT, { .i64 = -1 }, -1, INT_MAX, FLAGS },
    { NULL },
};

static const AVClass whep_demuxer_class = {
    .class_name = "WHEP demuxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const AVInputFormat ff_whep_demuxer = {
    .name             = "whep",
    .long_name        = NULL_IF_CONFIG_SMALL("WebRTC-HTTP egress protocol (WHEP) demuxer"),
    .flags            = AVFMT_NOFILE | AVFMT_EXPERIMENTAL,
    .priv_class       = &whep_demuxer_class,
    .priv_data_size   = sizeof(WHEPContext),
    .read_header      = whep_read_header,
    .read_packet      = whep_read_packet,
    .read_close       = whep_read_close,
};
