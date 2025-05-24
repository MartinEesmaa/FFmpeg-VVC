/*
 * WebRTC-HTTP ingestion/egress protocol (WHIP/WHEP) common code
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

#ifndef AVFORMAT_WEBRTC_H
#define AVFORMAT_WEBRTC_H

#include "avformat.h"
#include "avio_internal.h"
#include "libavcodec/codec_id.h"
#include "url.h"
#include "rtc/rtc.h"
#include "libavutil/thread.h"

#define RTP_MAX_PACKET_SIZE 1200

typedef struct WebRTCTrack {
    AVFormatContext *avctx;
    int track_id;
    int stream_index;
    AVFormatContext *rtp_ctx;
    URLContext *rtp_url_context;
    pthread_t thread;
} WebRTCTrack;

typedef struct WebRTCContext {
    AVFormatContext *avctx;
    int peer_connection;
    rtcState state;
    WebRTCTrack *tracks;
    int nb_tracks;
    const char *resource_location;

    /* options */
    char* bearer_token;
    int64_t connection_timeout;
    int64_t rw_timeout;
} WebRTCContext;

#define FF_WEBRTC_COMMON_OPTIONS \
    { "bearer_token", "optional bearer token for authentication and authorization", OFFSET(webrtc_ctx.bearer_token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS }, \
    { "connection_timeout", "timeout for establishing a connection", OFFSET(webrtc_ctx.connection_timeout), AV_OPT_TYPE_DURATION, { .i64 = 10000000 }, 1, INT_MAX, FLAGS }, \
    { "rw_timeout", "timeout for receiving/writing data", OFFSET(webrtc_ctx.rw_timeout), AV_OPT_TYPE_DURATION, { .i64 = 1000000 }, 1, INT_MAX, FLAGS }

int ff_webrtc_close_resource(WebRTCContext*const ctx);
int ff_webrtc_convert_codec(enum AVCodecID codec_id, rtcCodec* rtc_codec);
int ff_webrtc_create_resource(WebRTCContext*const ctx);
void ff_webrtc_deinit(WebRTCContext*const ctx, bool is_muxer);
int ff_webrtc_generate_media_stream_id(char media_stream_id[37]);
int ff_webrtc_init_connection(WebRTCContext*const ctx);
void ff_webrtc_init_logger(void);
int ff_webrtc_init_urlcontext(WebRTCContext*const ctx, int track_idx);

#endif /* AVFORMAT_WEBRTC_H */
