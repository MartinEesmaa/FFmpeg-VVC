/*
 * WebRTC-HTTP ingestion protocol (WHIP) muxer using libdatachannel
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

#include "avformat.h"
#include "internal.h"
#include "libavutil/avstring.h"
#include "libavutil/time.h"
#include "mux.h"
#include "rtpenc.h"
#include "rtpenc_chain.h"
#include "rtsp.h"
#include "webrtc.h"
#include "version.h"

typedef struct WHIPContext {
    AVClass *av_class;
    WebRTCContext webrtc_ctx;
} WHIPContext;


static void whip_deinit(AVFormatContext* avctx);
static int whip_init(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    AVStream* stream;
    const AVCodecParameters* codecpar;
    int i, ret;
    char media_stream_id[37] = { 0 };
    rtcTrackInit track_init;
    const AVChannelLayout supported_layout = AV_CHANNEL_LAYOUT_STEREO;
    const RTPMuxContext* rtp_mux_ctx;
    WebRTCTrack* track;
    char sdp_stream[SDP_MAX_SIZE] = { 0 };
    char* fmtp;

    ctx->webrtc_ctx.avctx = avctx;
    ff_webrtc_init_logger();
    ret = ff_webrtc_init_connection(&ctx->webrtc_ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to initialize connection\n");
        goto fail;
    }

    if (!(ctx->webrtc_ctx.tracks = av_mallocz(sizeof(WebRTCTrack) * avctx->nb_streams))) {
        av_log(avctx, AV_LOG_ERROR, "Failed to allocate tracks\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* configure tracks */
    ret = ff_webrtc_generate_media_stream_id(media_stream_id);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to generate media stream id\n");
        goto fail;
    }

    for (i = 0; i < avctx->nb_streams; ++i) {
        stream = avctx->streams[i];
        codecpar = stream->codecpar;
        track = &ctx->webrtc_ctx.tracks[i];
        track->stream_index = i;

        switch (codecpar->codec_type)
        {
            case AVMEDIA_TYPE_VIDEO:
                /* based on rtpenc */
                avpriv_set_pts_info(stream, 32, 1, 90000);
                break;
            case AVMEDIA_TYPE_AUDIO:
                if (codecpar->sample_rate != 48000) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported sample rate. Only 48kHz is supported\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
                }
                if (av_channel_layout_compare(&codecpar->ch_layout, &supported_layout) != 0) {
                    av_log(avctx, AV_LOG_ERROR, "Unsupported channel layout. Only stereo is supported\n");
                    ret = AVERROR(EINVAL);
                    goto fail;
                }
                /* based on rtpenc */
                avpriv_set_pts_info(stream, 32, 1, codecpar->sample_rate);
                break;
            default:
                continue;
        }

        ret = ff_webrtc_init_urlcontext(&ctx->webrtc_ctx, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "webrtc_init_urlcontext failed\n");
            goto fail;
        }

        ret = ff_rtp_chain_mux_open(&track->rtp_ctx, avctx, stream, track->rtp_url_context, RTP_MAX_PACKET_SIZE, i);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "ff_rtp_chain_mux_open failed\n");
            goto fail;
        }
        rtp_mux_ctx = (const RTPMuxContext*)ctx->webrtc_ctx.tracks[i].rtp_ctx->priv_data;

        memset(&track_init, 0, sizeof(rtcTrackInit));
        track_init.direction = RTC_DIRECTION_SENDONLY;
        track_init.payloadType = rtp_mux_ctx->payload_type;
        track_init.ssrc = rtp_mux_ctx->ssrc;
        track_init.mid = av_asprintf("%d", i);
        track_init.name = LIBAVFORMAT_IDENT;
        track_init.msid = media_stream_id;
        track_init.trackId = av_asprintf("%s-video-%d", media_stream_id, i);

        ret = ff_webrtc_convert_codec(codecpar->codec_id, &track_init.codec);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to convert codec\n");
            goto fail;
        }

        /* parse fmtp from global header */
        ret = ff_sdp_write_media(sdp_stream, sizeof(sdp_stream), stream, i, NULL, NULL, 0, 0, NULL);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to write sdp\n");
            goto fail;
        }
        fmtp = strstr(sdp_stream, "a=fmtp:");
        if (fmtp) {
            track_init.profile = av_strndup(fmtp + 10, strchr(fmtp, '\r') - fmtp - 10);
            track_init.profile = av_asprintf("%s;level-asymmetry-allowed=1", track_init.profile);
            memset(sdp_stream, 0, sizeof(sdp_stream));
        }

        track->track_id = rtcAddTrackEx(ctx->webrtc_ctx.peer_connection, &track_init);
        if (track->track_id < 0) {
            av_log(avctx, AV_LOG_ERROR, "Failed to add track\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }

        if (track_init.profile)
            av_freep(&track_init.profile);
        if (track_init.mid)
            av_freep(&track_init.mid);
        if (track_init.trackId)
            av_freep(&track_init.trackId);
    }

    return 0;

fail:
    if (track_init.profile)
        av_freep(&track_init.profile);
    if (track_init.mid)
        av_freep(&track_init.mid);
    if (track_init.trackId)
        av_freep(&track_init.trackId);
    return ret;
}

static int whip_write_header(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    int ret;
    int64_t timeout;

    ret = ff_webrtc_create_resource(&ctx->webrtc_ctx);
    if (ret < 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create resource\n");
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

    return 0;

fail:
    return ret;
}

static int whip_write_packet(AVFormatContext* avctx, AVPacket* pkt)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    AVFormatContext* rtpctx = ctx->webrtc_ctx.tracks[pkt->stream_index].rtp_ctx;
    pkt->stream_index = 0;

    if (ctx->webrtc_ctx.state != RTC_CONNECTED) {
        av_log(avctx, AV_LOG_ERROR, "Connection is not open\n");
        return AVERROR(EINVAL);
    }

    return av_write_frame(rtpctx, pkt);
}

static int whip_write_trailer(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    return ff_webrtc_close_resource(&ctx->webrtc_ctx);
}

static void whip_deinit(AVFormatContext* avctx)
{
    WHIPContext*const ctx = (WHIPContext*const)avctx->priv_data;
    ff_webrtc_deinit(&ctx->webrtc_ctx, true);
}

static int whip_check_bitstream(AVFormatContext *s, AVStream *st, const AVPacket *pkt)
{
    /* insert SPS/PPS into every keyframe otherwise browsers won't play the stream */
    if (st->codecpar->extradata_size && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        return ff_stream_add_bitstream_filter(st, "dump_extra", "freq=keyframe");
    return 1;
}

static int whip_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    switch (codec_id)
    {
        case AV_CODEC_ID_OPUS:
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_PCM_MULAW:
        case AV_CODEC_ID_PCM_ALAW:
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        case AV_CODEC_ID_AV1:
        case AV_CODEC_ID_VP9:
            return 1;
        default:
            return 0;
    }
}

#define OFFSET(x) offsetof(WHIPContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    FF_WEBRTC_COMMON_OPTIONS,
    { NULL },
};

static const AVClass whip_muxer_class = {
    .class_name = "WHIP muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_whip_muxer = {
    .p.name             = "whip",
    .p.long_name        = NULL_IF_CONFIG_SMALL("WebRTC-HTTP ingestion protocol (WHIP) muxer"),
    .p.audio_codec      = AV_CODEC_ID_OPUS, // supported by major browsers
    .p.video_codec      = AV_CODEC_ID_H264,
    .p.flags            = AVFMT_NOFILE | AVFMT_GLOBALHEADER | AVFMT_EXPERIMENTAL,
    .p.priv_class       = &whip_muxer_class,
    .priv_data_size     = sizeof(WHIPContext),
    .write_packet       = whip_write_packet,
    .write_header       = whip_write_header,
    .write_trailer      = whip_write_trailer,
    .init               = whip_init,
    .deinit             = whip_deinit,
    .query_codec        = whip_query_codec,
    .check_bitstream    = whip_check_bitstream,
};
