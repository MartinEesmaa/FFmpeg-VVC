/**
 * WHIP (WebRTC-HTTP Ingestion Protocol) muxer
 * Copyright (c) 2025
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
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <rtc/rtc.h>
#include <stdatomic.h>
#include "avformat.h"
#include "libavutil/avstring.h"
#include "libavutil/lfg.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavutil/random_seed.h"
#include "mux.h"
#include "rtp.h"
#include "whip_whep.h"

typedef struct WHIPContext {
    AVClass *class;
    char *token;
    char *session_url;

    // libdatachannel state
    int pc;
    int *tracks;
} WHIPContext;

static int whip_write_header(AVFormatContext *s)
{
    WHIPContext *whip = s->priv_data;
    rtcConfiguration config = {0};
    AVLFG lfg;
    char msid[32], cname[32];

    ff_whip_whep_init_rtc_logger();

    // Initialize peer connection
    memset(&config, 0, sizeof(config));
    whip->pc = rtcCreatePeerConnection(&config);
    if (whip->pc <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create peer connection\n");
        return AVERROR_EXTERNAL;
    }
    rtcSetUserPointer(whip->pc, whip);

    // Add tracks based on stream codecs
    whip->tracks = av_calloc(s->nb_streams, sizeof(*whip->tracks));
    if (!whip->tracks)
        return AVERROR(ENOMEM);

    av_lfg_init(&lfg, av_get_random_seed());
    snprintf(msid, sizeof(msid), "stream-%08x", av_lfg_get(&lfg));
    snprintf(cname, sizeof(cname), "ffmpeg-%08x", av_lfg_get(&lfg));

    for (unsigned int i = 0, ssrc = av_get_random_seed(); i < s->nb_streams; i++, ssrc++) {
        int ret = 0;
        AVStream *st = s->streams[i];
        rtcTrackInit init = {0};
        rtcPacketizerInit pinit = {0};
        char *mid = NULL, *track_id = NULL;
        int pt = ff_rtp_get_payload_type(NULL, st->codecpar, i);

        if (ssrc == 0) ssrc++;

        init.direction   = RTC_DIRECTION_SENDONLY;
        init.payloadType = pt;
        init.ssrc        = ssrc;
        init.mid         = mid = av_asprintf("%d", i);
        init.name        = cname;
        init.msid        = msid;
        init.trackId     = track_id = av_asprintf("track-%d", i);

        switch (st->codecpar->codec_id) {
        case AV_CODEC_ID_H264:
            init.codec    = RTC_CODEC_H264;
            init.profile  = "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f";
            st->time_base = (AVRational){1, 90000};
            break;
        case AV_CODEC_ID_H265:
            init.codec    = RTC_CODEC_H265;
            st->time_base = (AVRational){1, 90000};
            break;
        case AV_CODEC_ID_OPUS:
            init.codec    = RTC_CODEC_OPUS;
            init.profile  = "minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1";
            st->time_base = (AVRational){1, 48000};
            break;
        case AV_CODEC_ID_PCM_ALAW:
            if (st->codecpar->sample_rate != 8000 || st->codecpar->ch_layout.nb_channels != 1) {
                av_log(s, AV_LOG_ERROR, "Unsupported PCMA format %d/%d. Try adding `-ar 8000 -ac 1`.\n",
                    st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            init.codec    = RTC_CODEC_PCMA;
            st->time_base = (AVRational){1, 8000};
            break;
        case AV_CODEC_ID_PCM_MULAW:
            if (st->codecpar->sample_rate != 8000 || st->codecpar->ch_layout.nb_channels != 1) {
                av_log(s, AV_LOG_ERROR, "Unsupported PCMU format %d/%d. Try adding `-ar 8000 -ac 1`.\n",
                    st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            init.codec    = RTC_CODEC_PCMU;
            st->time_base = (AVRational){1, 8000};
            break;
        case AV_CODEC_ID_ADPCM_G722:
            if (st->codecpar->sample_rate != 16000 || st->codecpar->ch_layout.nb_channels!= 1) {
                av_log(s, AV_LOG_ERROR, "Unsupported G722 format %d/%d. Try adding `-ar 16000 -ac 1`.\n",
                    st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels);
                ret = AVERROR(EINVAL);
                goto fail;
            }
            init.codec    = RTC_CODEC_G722;
            st->time_base = (AVRational){1, 8000};
            break;
        default:
            av_log(s, AV_LOG_ERROR, "Unsupported codec\n");
            ret = AVERROR(EINVAL);
            goto fail;
        }

        whip->tracks[i] = rtcAddTrackEx(whip->pc, &init);
        if (whip->tracks[i] <= 0) {
            av_log(s, AV_LOG_ERROR, "Failed to add track\n");
            ret = AVERROR_EXTERNAL;
            goto fail;
        }

        // Setup packetizer
        pinit.payloadType = pt;
        pinit.ssrc       = ssrc;
        pinit.cname      = cname;

        switch (st->codecpar->codec_id) {
        case AV_CODEC_ID_H264:
            pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
            if (rtcSetH264Packetizer(whip->tracks[i], &pinit) < 0) {
                av_log(s, AV_LOG_ERROR, "Failed to set H264 packetizer\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            break;
        case AV_CODEC_ID_H265:
            pinit.nalSeparator = RTC_NAL_SEPARATOR_START_SEQUENCE;
            if (rtcSetH265Packetizer(whip->tracks[i], &pinit) < 0) {
                av_log(s, AV_LOG_ERROR, "Failed to set H265 packetizer\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            break;
        case AV_CODEC_ID_OPUS:
            if (rtcSetOpusPacketizer(whip->tracks[i], &pinit) < 0) {
                av_log(s, AV_LOG_ERROR, "Failed to set Opus packetizer\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            break;
        case AV_CODEC_ID_PCM_ALAW:
            if (rtcSetPCMAPacketizer(whip->tracks[i], &pinit) < 0) {
                av_log(s, AV_LOG_ERROR, "Failed to set PCMA packetizer\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            break;
        case AV_CODEC_ID_PCM_MULAW:
            if (rtcSetPCMUPacketizer(whip->tracks[i], &pinit) < 0) {
                av_log(s, AV_LOG_ERROR, "Failed to set PCMU packetizer\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            break;
        case AV_CODEC_ID_ADPCM_G722:
            if (rtcSetG722Packetizer(whip->tracks[i], &pinit) < 0) {
                av_log(s, AV_LOG_ERROR, "Failed to set G722 packetizer\n");
                ret = AVERROR_EXTERNAL;
                goto fail;
            }
            break;
        }

        rtcChainRtcpSrReporter(whip->tracks[i]);
        rtcChainRtcpNackResponder(whip->tracks[i], 512);

        av_free(mid);
        av_free(track_id);
        continue;

fail:
        av_free(mid);
        av_free(track_id);
        return ret;
    }

    return ff_whip_whep_exchange_and_set_sdp(s, whip->pc, whip->token, &whip->session_url);
}

static int whip_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHIPContext *whip = s->priv_data;
    int track;

    if (pkt->stream_index >= s->nb_streams)
        return AVERROR(EINVAL);

    track = whip->tracks[pkt->stream_index];

    if (rtcIsClosed(track))
        return AVERROR_EOF;

    if (rtcIsOpen(track)) {
        rtcSetTrackRtpTimestamp(track, pkt->pts);
        if (rtcSendMessage(track, (const char *)pkt->data, pkt->size) < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to send frame\n");
            return AVERROR_EXTERNAL;
        }
    }

    return 0;
}

static void whip_deinit(AVFormatContext *s)
{
    WHIPContext *whip = s->priv_data;

    if (whip->tracks) {
        for (int i = 0; i < s->nb_streams; i++) {
            if (whip->tracks[i] > 0) {
                rtcDeleteTrack(whip->tracks[i]);
                whip->tracks[i] = 0;
            }
        }
        av_freep(&whip->tracks);
    }

    if (whip->pc > 0) {
        rtcDeletePeerConnection(whip->pc);
        whip->pc = 0;
    }

    if (whip->session_url) {
        ff_whip_whep_delete_session(s, whip->token, whip->session_url);
        av_freep(&whip->session_url);
    }
}

static int whip_query_codec(enum AVCodecID id, int std_compliance)
{
    switch (id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_H265:
    case AV_CODEC_ID_OPUS:
    case AV_CODEC_ID_PCM_ALAW:
    case AV_CODEC_ID_PCM_MULAW:
    case AV_CODEC_ID_ADPCM_G722:
        return 1;
    default:
        return 0;
    }
}

#define OFFSET(x) offsetof(WHIPContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption whip_options[] = {
    { "token", "set token to send in the Authorization header as \"Bearer <token>\"",
        OFFSET(token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL }
};

static const AVClass whip_class = {
    .class_name = "WHIP muxer",
    .item_name  = av_default_item_name,
    .option     = whip_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_whip_emuxer = {
    .p.name         = "whip",
    .p.long_name    = NULL_IF_CONFIG_SMALL("WHIP (WebRTC-HTTP Ingestion Protocol)"),
    .p.audio_codec  = AV_CODEC_ID_OPUS,
    .p.video_codec  = AV_CODEC_ID_H264,
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &whip_class,
    .priv_data_size = sizeof(WHIPContext),
    .write_header   = whip_write_header,
    .write_packet   = whip_write_packet,
    .deinit         = whip_deinit,
    .query_codec    = whip_query_codec,
};