/**
 * WHEP (WebRTC-HTTP Egress Protocol) demuxer
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
#include "demux.h"
#include "libavcodec/codec_id.h"
#include "libavutil/avstring.h"
#include "libavutil/opt.h"
#include "libavutil/mem.h"
#include "libavutil/time.h"
#include "libavutil/random_seed.h"
#include "rtpdec.h"
#include "whip_whep.h"

static const struct {
    int pt;
    const char enc_name[6];
    enum AVMediaType codec_type;
    enum AVCodecID codec_id;
    int clock_rate;
    int audio_channels;
} dynamic_payload_types[] = {
  {96, "VP8", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VP8,  90000, -1},
  {97, "VP9", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_VP9,  90000, -1},
  {98, "H264", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 90000, -1},
  {99, "H265", AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H265, 90000, -1},
  {111, "OPUS", AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_OPUS, 48000, 2},
  {-1, "", AVMEDIA_TYPE_UNKNOWN, AV_CODEC_ID_NONE, -1, -1}
};

static const char *audio_mline =
    "m=audio 9 UDP/TLS/RTP/SAVPF 111 9 0 8\n"
    "a=mid:0\n"
    "a=recvonly\n"
    "a=rtpmap:111 opus/48000/2\n"
    "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1\n"
    "a=rtpmap:9 G722/8000\n"
    "a=rtpmap:0 PCMU/8000\n"
    "a=rtpmap:8 PCMA/8000\n";

static const char *video_mline =
    "m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99\n"
    "a=mid:1\n"
    "a=recvonly\n"
    "a=rtpmap:96 VP8/90000\n"
    "a=rtcp-fb:96 goog-remb\n"
    "a=rtcp-fb:96 nack\n"
    "a=rtcp-fb:96 nack pli\n"
    "a=rtpmap:97 VP9/90000\n"
    "a=rtcp-fb:97 goog-remb\n"
    "a=rtcp-fb:97 nack\n"
    "a=rtcp-fb:97 nack pli\n"
    "a=rtpmap:98 H264/90000\n"
    "a=rtcp-fb:98 goog-remb\n"
    "a=rtcp-fb:98 nack\n"
    "a=rtcp-fb:98 nack pli\n"
    "a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\n"
    "a=rtpmap:99 H265/90000\n"
    "a=rtcp-fb:99 goog-remb\n"
    "a=rtcp-fb:99 nack\n"
    "a=rtcp-fb:99 nack pli\n";

typedef struct Message {
    int track;
    uint8_t *data;
    int size;
} Message;

typedef struct WHEPContext {
    AVClass *class;
    char *token;
    char *session_url;
    int64_t pli_period;
    int64_t last_pli_time;

    // libdatachannel state
    int pc;
    int audio_track;
    int video_track;

    RTPDemuxContext **rtp_ctxs;
    int rtp_ctxs_count;

    // lock-free ring buffer for messages (rtp packets)
    Message **buffer;
    int capacity;
    atomic_int head;
    atomic_int tail;

    AVPacket *audio_pkt;
    AVPacket *video_pkt;
} WHEPContext;

static int whep_get_sdp_a_line(int track, char *buffer, int size, int payload_type)
{
    char *line, *end;
    char fmtp_prefix[16];

    if (rtcGetTrackDescription(track, buffer, size) < 0)
        return AVERROR_EXTERNAL;
    line = buffer;
    end  = buffer + strlen(buffer);
    snprintf(fmtp_prefix, sizeof(fmtp_prefix), "a=fmtp:%d", payload_type);

    while (line < end) {
        char *next_line = strchr(line, '\n');
        if (next_line)
            *next_line = '\0';

        while (*line == ' ' || *line == '\t')
            line++;

        if (av_strstart(line, fmtp_prefix, NULL)) {
            av_strlcpy(buffer, line + 2, size);
            return 0;
        }

        if (next_line) {
            *next_line = '\n';
            line = next_line + 1;
        } else {
            break;
        }
    }

    buffer[0] = '\0';
    return AVERROR(ENOENT);
}

static RTPDemuxContext* whep_new_rtp_context(AVFormatContext *s, int payload_type)
{
    WHEPContext *whep = s->priv_data;
    RTPDemuxContext **rtp_ctxs = NULL;
    RTPDemuxContext *rtp_ctx = NULL;
    AVStream *st = NULL;
    const RTPDynamicProtocolHandler *handler = NULL;
    PayloadContext *dynamic_protocol_context = NULL;

    rtp_ctxs = av_realloc_array(whep->rtp_ctxs, whep->rtp_ctxs_count + 1,
                                           sizeof(*whep->rtp_ctxs));
    if (!rtp_ctxs) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate RTP context array\n");
        goto fail;
    }
    whep->rtp_ctxs = rtp_ctxs;

    st = avformat_new_stream(s, NULL);
    if (!st) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate stream\n");
        goto fail;
    }
    if (ff_rtp_get_codec_info(st->codecpar, payload_type) < 0) {
        for (int i = 0; dynamic_payload_types[i].pt > 0; i++) {
            if (dynamic_payload_types[i].pt == payload_type) {
                st->codecpar->codec_id   = dynamic_payload_types[i].codec_id;
                st->codecpar->codec_type = dynamic_payload_types[i].codec_type;

                if (dynamic_payload_types[i].audio_channels > 0) {
                    av_channel_layout_uninit(&st->codecpar->ch_layout);
                    st->codecpar->ch_layout.order       = AV_CHANNEL_ORDER_UNSPEC;
                    st->codecpar->ch_layout.nb_channels = dynamic_payload_types[i].audio_channels;
                }
                if (dynamic_payload_types[i].clock_rate > 0)
                    st->codecpar->sample_rate = dynamic_payload_types[i].clock_rate;
                handler = ff_rtp_handler_find_by_name(dynamic_payload_types[i].enc_name,
                                                     dynamic_payload_types[i].codec_type);
                break;
            }
        }
    }
    if (st->codecpar->sample_rate > 0)
        st->time_base = (AVRational){1, st->codecpar->sample_rate};

    rtp_ctx = ff_rtp_parse_open(s, st, payload_type, RTP_REORDER_QUEUE_DEFAULT_SIZE);
    if (!rtp_ctx) {
        av_log(s, AV_LOG_ERROR, "Failed to open RTP context\n");
        goto fail;
    }
    if (handler) {
        ffstream(st)->need_parsing = handler->need_parsing;
        dynamic_protocol_context = av_mallocz(handler->priv_data_size);
        if (!dynamic_protocol_context) {
            av_log(s, AV_LOG_ERROR, "Failed to allocate dynamic protocol context\n");
            goto fail;
        }
        if (handler->init && handler->init(s, st->index, dynamic_protocol_context) < 0) {
            av_log(s, AV_LOG_ERROR, "Failed to initialize dynamic protocol context\n");
            goto fail;
        }
        ff_rtp_parse_set_dynamic_protocol(rtp_ctx, dynamic_protocol_context, handler);

        if (handler->parse_sdp_a_line) {
            char line[SDP_MAX_SIZE];
            int track_id = (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ?
                          whep->audio_track : whep->video_track;
            if (whep_get_sdp_a_line(track_id, line, sizeof(line), payload_type) < 0) {
                av_log(s, AV_LOG_WARNING, "No SDP a-line for payload type %d\n", payload_type);
            } else {
                handler->parse_sdp_a_line(s, st->index, dynamic_protocol_context, line);
            }
        }
    }

    whep->rtp_ctxs[whep->rtp_ctxs_count++] = rtp_ctx;
    return rtp_ctx;

fail:
    if (rtp_ctx)
        ff_rtp_parse_close(rtp_ctx);
    av_free(dynamic_protocol_context);
    return NULL;
}

static void message_callback(int id, const char *message, int size, void *ptr)
{
    WHEPContext *whep = ptr;
    Message *msg;
    int current_head, next, current_tail;

    if (size < 2)
        return;

    if (RTP_PT_IS_RTCP(message[1]) && size < 8 || size < 12)
        return;

    // Push packet to ring buffer
    msg = av_malloc(sizeof(Message));
    if (!msg) {
        av_log(whep, AV_LOG_ERROR, "Failed to allocate message\n");
        return;
    }
    msg->track = id;
    msg->data  = av_memdup(message, size);
    msg->size  = size;

    if (!msg->data) {
        av_log(whep, AV_LOG_ERROR, "Failed to duplicate message\n");
        av_free(msg);
        return;
    }
    current_tail = atomic_load_explicit(&whep->tail, memory_order_relaxed);
    next         = (current_tail + 1) % whep->capacity;
    current_head = atomic_load_explicit(&whep->head, memory_order_acquire);

    if (next == current_head) {
        av_log(whep, AV_LOG_ERROR, "Message buffer is full\n");
        av_free(msg->data);
        av_free(msg);
        return;
    }

    whep->buffer[current_tail] = msg;
    atomic_store_explicit(&whep->tail, next, memory_order_release);
}

static int whep_read_header(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;
    rtcConfiguration config = {0};

    ff_whip_whep_init_rtc_logger();
    s->ctx_flags |= AVFMTCTX_NOHEADER;

    whep->capacity = 1024;
    whep->buffer = av_calloc(whep->capacity, sizeof(*whep->buffer));
    if (!whep->buffer) {
        av_log(s, AV_LOG_ERROR, "Failed to allocate message buffer\n");
        return AVERROR(ENOMEM);
    }
    // Initialize WebRTC peer connection
    whep->pc = rtcCreatePeerConnection(&config);
    if (whep->pc <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create peer connection\n");
        return AVERROR_EXTERNAL;
    }
    rtcSetUserPointer(whep->pc, whep);

    // Add audio and video track
    whep->audio_track = rtcAddTrack(whep->pc, audio_mline);
    if (whep->audio_track <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to add audio track\n");
        return AVERROR_EXTERNAL;
    }

    if (rtcSetMessageCallback(whep->audio_track, message_callback) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set audio track message callback\n");
        return AVERROR_EXTERNAL;
    }

    whep->video_track = rtcAddTrack(whep->pc, video_mline);
    if (whep->video_track <= 0) {
        av_log(s, AV_LOG_ERROR, "Failed to add video track\n");
        return AVERROR_EXTERNAL;
    }

    if (rtcSetMessageCallback(whep->video_track, message_callback) < 0) {
        av_log(s, AV_LOG_ERROR, "Failed to set video track message callback\n");
        return AVERROR_EXTERNAL;
    }

    return ff_whip_whep_exchange_and_set_sdp(s, whep->pc, whep->token, &whep->session_url);
}

static int whep_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    WHEPContext *whep = s->priv_data;
    int current_head, current_tail, ret;
    Message *msg = NULL;
    RTPDemuxContext *rtp_ctx = NULL;
    AVIOContext *dyn_bc = NULL;
    if (!whep->audio_pkt)
        whep->audio_pkt = av_packet_alloc();
    if (!whep->video_pkt)
        whep->video_pkt = av_packet_alloc();

redo:
    rtp_ctx = NULL;
    if (msg) {
        av_free(msg->data);
        av_free(msg);
    }
    if (rtcIsClosed(whep->audio_track) || rtcIsClosed(whep->video_track)) {
        av_log(s, AV_LOG_ERROR, "Connection closed\n");
        return AVERROR_EOF;
    }

    current_head = atomic_load_explicit(&whep->head, memory_order_relaxed);
    current_tail = atomic_load_explicit(&whep->tail, memory_order_acquire);

    if (current_head == current_tail)  // empty
        return AVERROR(EAGAIN);
    msg = whep->buffer[current_head];
    atomic_store_explicit(&whep->head, (current_head + 1) % whep->capacity,
                         memory_order_release);

    if (RTP_PT_IS_RTCP(msg->data[1])) {
        switch(msg->data[1]) {
        case RTCP_SR:
            uint32_t ssrc = (msg->data[4] << 24) | (msg->data[5] << 16) | (msg->data[6] << 8) | msg->data[7];
            for (int i = 0; i < whep->rtp_ctxs_count; i++) {
                if (whep->rtp_ctxs[i]->ssrc == ssrc) {
                    rtp_ctx = whep->rtp_ctxs[i];
                    break;
                }
            }
            // Send RTCP RR
            if (rtp_ctx && avio_open_dyn_buf(&dyn_bc) == 0) {
                int len;
                uint8_t *dyn_buf = NULL;
                ff_rtp_check_and_send_back_rr(rtp_ctx, NULL, dyn_bc, 300000);
                len = avio_close_dyn_buf(dyn_bc, &dyn_buf);
                if (len > 0 && dyn_buf && rtcSendMessage(msg->track, dyn_buf, len) < 0)
                    av_log(s, AV_LOG_ERROR, "Failed to send RTCP RR\n");
                av_free(dyn_buf);
            }
            break;
        default:
            goto redo;
        }
    } else {
        int payload_type = msg->data[1] & 0x7f;
        for (int i = 0; i < whep->rtp_ctxs_count; i++) {
            if (whep->rtp_ctxs[i]->payload_type == payload_type) {
                rtp_ctx = whep->rtp_ctxs[i];
                break;
            }
        }

        if (!rtp_ctx) {
            AVCodecParameters par;
            ret = ff_rtp_get_codec_info(&par, payload_type);
            if (ret < 0) {
                for (int i = 0; dynamic_payload_types[i].pt > 0; i++) {
                    if (dynamic_payload_types[i].pt == payload_type) {
                        ret = 0;
                        break;
                    }
                }
            }
            if (ret == 0) {
                av_log(s, AV_LOG_DEBUG, "Create RTP context for payload type %d\n", payload_type);
                rtp_ctx = whep_new_rtp_context(s, payload_type);
            }
        }
    }

    if (!rtp_ctx) {
        av_log(s, AV_LOG_WARNING, "Failed to get RTP context for message %d\n", msg->data[1]);
        goto redo;
    }

    // Parse RTP packet
    if (msg->track == whep->audio_track)
        ret = ff_rtp_parse_packet(rtp_ctx, whep->audio_pkt, (uint8_t **)&msg->data, msg->size) < 0;
    else if (msg->track == whep->video_track)
        ret = ff_rtp_parse_packet(rtp_ctx, whep->video_pkt, (uint8_t **)&msg->data, msg->size) < 0;

    // Send RTCP feedback
    if (avio_open_dyn_buf(&dyn_bc) == 0) {
        int len;
        uint8_t *dyn_buf = NULL;
        ff_rtp_send_rtcp_feedback(rtp_ctx, NULL, dyn_bc, 1);
        len = avio_close_dyn_buf(dyn_bc, &dyn_buf);
        if (len > 0 && dyn_buf && rtcSendMessage(msg->track, dyn_buf, len) < 0)
            av_log(s, AV_LOG_ERROR, "Failed to send RTCP feedback\n");
        av_free(dyn_buf);
    }

    // Send PLI
    if (msg->track == whep->video_track && rtp_ctx->ssrc) {
        int64_t now = av_gettime_relative();
        if ((whep->pli_period && now - whep->last_pli_time >= whep->pli_period * 1000000) ||
            (rtp_ctx->handler && rtp_ctx->handler->need_keyframe &&
            rtp_ctx->handler->need_keyframe(rtp_ctx->dynamic_protocol_context))) {
            uint32_t source_ssrc = rtp_ctx->ssrc;
            uint32_t sender_ssrc = source_ssrc + 1;
            uint8_t pli_packet[] = {
                (RTP_VERSION << 6) | 1, RTCP_PSFB,         0x00,             0x02,
                sender_ssrc >> 24,      sender_ssrc >> 16, sender_ssrc >> 8, sender_ssrc,
                source_ssrc >> 24,      source_ssrc >> 16, source_ssrc >> 8, source_ssrc,
            };
            if (rtcSendMessage(msg->track, pli_packet, sizeof(pli_packet)) < 0)
                av_log(s, AV_LOG_ERROR, "Failed to send PLI\n");
            else
                whep->last_pli_time = now;
        }
    }

    if (ret != 0)
        goto redo;

    if (msg->track == whep->audio_track) {
        av_packet_ref(pkt, whep->audio_pkt);
        av_packet_free(&whep->audio_pkt);
    } else if (msg->track == whep->video_track) {
        av_packet_ref(pkt, whep->video_pkt);
        av_packet_free(&whep->video_pkt);
    }
    av_free(msg->data);
    av_free(msg);
    return 0;
}

static int whep_read_close(AVFormatContext *s)
{
    WHEPContext *whep = s->priv_data;

    if (whep->audio_track > 0) {
        rtcDeleteTrack(whep->audio_track);
        whep->audio_track = 0;
    }
    if (whep->video_track > 0) {
        rtcDeleteTrack(whep->video_track);
        whep->video_track = 0;
    }
    if (whep->pc > 0) {
        rtcDeletePeerConnection(whep->pc);
        whep->pc = 0;
    }

    if (whep->rtp_ctxs) {
        for (int i = 0; i < whep->rtp_ctxs_count; i++) {
            if (whep->rtp_ctxs[i]) {
                PayloadContext *payload_ctx = whep->rtp_ctxs[i]->dynamic_protocol_context;
                ff_rtp_parse_close(whep->rtp_ctxs[i]);
                av_freep(&payload_ctx);
            }
        }
        av_freep(&whep->rtp_ctxs);
        whep->rtp_ctxs_count = 0;
    }

    if (whep->buffer) {
        int head = atomic_load(&whep->head);
        int tail = atomic_load(&whep->tail);

        while (head != tail) {
            Message *msg = whep->buffer[head];
            if (msg) {
                av_freep(&msg->data);
                av_freep(&msg);
            }
            head = (head + 1) % whep->capacity;
        }
        av_freep(&whep->buffer);
    }

    if (whep->audio_pkt)
        av_packet_free(&whep->audio_pkt);
    if (whep->video_pkt)
        av_packet_free(&whep->video_pkt);

    if (whep->session_url) {
        ff_whip_whep_delete_session(s, whep->token, whep->session_url);
        av_freep(&whep->session_url);
    }

    return 0;
}

#define OFFSET(x) offsetof(WHEPContext, x)
static const AVOption whep_options[] = {
    { "token", "set token to send in the Authorization header as \"Bearer <token>\"",
        OFFSET(token), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, AV_OPT_FLAG_DECODING_PARAM },
    { "pli_period", "set interval in seconds for sending periodic PLI (Picture Loss Indication) requests; 0 to disable",
        OFFSET(pli_period), AV_OPT_TYPE_INT, {.i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_DECODING_PARAM },
    { NULL }
};

static const AVClass whep_class = {
    .class_name = "WHEP demuxer",
    .item_name  = av_default_item_name,
    .option     = whep_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFInputFormat ff_whep_edemuxer = {
    .p.name         = "whep",
    .p.long_name    = NULL_IF_CONFIG_SMALL("WHEP (WebRTC-HTTP Egress Protocol)"),
    .p.flags        = AVFMT_NOFILE,
    .p.priv_class   = &whep_class,
    .priv_data_size = sizeof(WHEPContext),
    .read_header    = whep_read_header,
    .read_packet    = whep_read_packet,
    .read_close     = whep_read_close,
    .flags_internal = FF_INFMT_FLAG_INIT_CLEANUP,
};