/*
 * RTP parser for VP9 payload format (RFC 9628) - experimental
 * Copyright (c) 2015 Thomas Volkert <thomas@homer-conferencing.com>
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

#include "libavutil/intreadwrite.h"

#include "avio_internal.h"
#include "rtpdec_formats.h"

#define RTP_VP9_DESC_REQUIRED_SIZE 1

struct PayloadContext {
    AVIOContext *buf;
    uint32_t     timestamp;
};

static av_cold int vp9_init(AVFormatContext *ctx, int st_index,
                            PayloadContext *data)
{
    av_log(ctx, AV_LOG_WARNING,
           "RTP/VP9 support is still experimental\n");

    return 0;
}

static int vp9_handle_packet(AVFormatContext *ctx, PayloadContext *rtp_vp9_ctx,
                             AVStream *st, AVPacket *pkt, uint32_t *timestamp,
                             const uint8_t *buf, int len, uint16_t seq,
                             int flags)
{
    int has_pic_id, has_layer_idc, has_ref_idc, has_ss_data;
    av_unused int pic_id = 0, non_key_frame = 0, inter_picture_layer_frame;
    av_unused int layer_temporal = -1, layer_spatial = -1;
    int first_fragment, last_fragment;
    int rtp_m;
    int res = 0;

    /* drop data of previous packets in case of non-continuous (lossy) packet stream */
    if (rtp_vp9_ctx->buf && rtp_vp9_ctx->timestamp != *timestamp)
        ffio_free_dyn_buf(&rtp_vp9_ctx->buf);

    /* sanity check for size of input packet: 1 byte payload at least */
    if (len < RTP_VP9_DESC_REQUIRED_SIZE + 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet, got %d bytes\n", len);
        return AVERROR_INVALIDDATA;
    }

    /*
     *     decode the required VP9 payload descriptor according to section 4.2 of the spec.:
     *
     *      0 1 2 3 4 5 6 7
     *     +-+-+-+-+-+-+-+-+
     *     |I|P|L|F|B|E|V|Z| (REQUIRED)
     *     +-+-+-+-+-+-+-+-+
     *
     *     I: Picture ID (PID) present
     *     P: Inter-picture predicted frame
     *     L: Layer indices present
     *     F: Flexible mode
     *     B: Start of Frame
     *     E: End of Frame
     *     V: Scalability Structure (SS) data present
     *     Z: Not a reference frame for upper spatial layers
     */
    has_pic_id     = !!(buf[0] & 0x80);
    inter_picture_layer_frame = !!(buf[0] & 0x40);
    has_layer_idc  = !!(buf[0] & 0x20);
    has_ref_idc    = !!(buf[0] & 0x10);
    first_fragment = !!(buf[0] & 0x08);
    last_fragment  = !!(buf[0] & 0x04);
    has_ss_data    = !!(buf[0] & 0x02);

    rtp_m = !!(flags & RTP_FLAG_MARKER);

    /* sanity check for markers: E should always be equal to the RTP M marker */
    if (last_fragment != rtp_m) {
        av_log(ctx, AV_LOG_ERROR, "Invalid combination of B and M marker (%d != %d)\n", last_fragment, rtp_m);
        return AVERROR_INVALIDDATA;
    }

    /* pass the extensions field */
    buf += RTP_VP9_DESC_REQUIRED_SIZE;
    len -= RTP_VP9_DESC_REQUIRED_SIZE;

    /*
     *         decode the 1-byte/2-byte picture ID:
     *
     *          0 1 2 3 4 5 6 7
     *         +-+-+-+-+-+-+-+-+
     *   I:    |M|PICTURE ID   | (RECOMMENDED)
     *         +-+-+-+-+-+-+-+-+
     *   M:    | EXTENDED PID  | (RECOMMENDED)
     *         +-+-+-+-+-+-+-+-+
     *
     *   M: The most significant bit of the first octet is an extension flag.
     *   PictureID:  8 or 16 bits including the M bit.
     */
    if (has_pic_id) {
        /* check for 1-byte or 2-byte picture index */
        if (buf[0] & 0x80) {
            if (len < 2) {
                av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
                return AVERROR_INVALIDDATA;
            }
            pic_id = AV_RB16(buf) & 0x7fff;
            buf += 2;
            len -= 2;
        } else {
            pic_id = buf[0] & 0x7f;
            buf++;
            len--;
        }
    }

    /*
     *         decode layer indices
     *
     *          0 1 2 3 4 5 6 7
     *         +-+-+-+-+-+-+-+-+
     *   L:    | TID |U| SID |D| (Conditionally RECOMMENDED)
     *         +-+-+-+-+-+-+-+-+
     *         |   TL0PICIDX   | (Conditionally REQUIRED)
     *         +-+-+-+-+-+-+-+-+
     *
     *   TID: Temporal layer ID (3 bits)
     *   U: Switching up point (1 bit)
     *   SID: Spatial layer ID (3 bits)
     *   D: Inter-layer dependency used (1 bit)
     *   TL0PICIDX: Temporal Layer 0 Picture Index (8 bits, non-flexible mode only)
     */
    if (has_layer_idc) {
        if (len < 1) {
            av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
            return AVERROR_INVALIDDATA;
        }
        layer_temporal = buf[0] >> 5;
        layer_spatial  = (buf[0] >> 1) & 0x07;
        buf++;
        len--;

        if (!has_ref_idc) {
            if (len < 1) {
                av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
                return AVERROR_INVALIDDATA;
            }
            /* ignore TL0PICIDX */
            buf++;
            len--;
        }
    }

    /*
     *         decode reference indices
     *
     *          0 1 2 3 4 5 6 7
     *         +-+-+-+-+-+-+-+-+                             -\
     *   P,F:  | P_DIFF      |N| (Conditionally REQUIRED)    - up to 3 times
     *         +-+-+-+-+-+-+-+-+                             -/
     *
     *   P_DIFF: Relative Picture ID (7 bits)
     *   N: 1 if another P_DIFF follows
     */
    if (has_ref_idc && inter_picture_layer_frame) {
        int i, p_diff, has_more;
        for (i = 0; i < 3; i++) {
            if (len < 1) {
                av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
                return AVERROR_INVALIDDATA;
            }

            p_diff = buf[0] >> 1;
            has_more = buf[0] & 0x01;

            if (!p_diff) {
                av_log(ctx, AV_LOG_ERROR, "Invalid P_DIFF value 0\n");
                return AVERROR_INVALIDDATA;
            }

            buf++;
            len--;

            if (!has_more)
                break;
        }
    }

    /*
     *         decode the scalability structure (SS)
     *
     *          0 1 2 3 4 5 6 7
     *         +-+-+-+-+-+-+-+-+
     *   V:    | N_S |Y|G|-|-|-|
     *         +-+-+-+-+-+-+-+-+              -\
     *   Y:    |     WIDTH     | (OPTIONAL)    .
     *         +               +               .
     *         |               | (OPTIONAL)    .
     *         +-+-+-+-+-+-+-+-+               . - N_S + 1 times
     *         |     HEIGHT    | (OPTIONAL)    .
     *         +               +               .
     *         |               | (OPTIONAL)    .
     *         +-+-+-+-+-+-+-+-+              -/
     *   G:    |      N_G      | (OPTIONAL)
     *         +-+-+-+-+-+-+-+-+                            -\
     *   N_G:  | TID |U| R |-|-| (OPTIONAL)                 .
     *         +-+-+-+-+-+-+-+-+              -\            . - N_G times
     *         |    P_DIFF     | (OPTIONAL)    . - R times  .
     *         +-+-+-+-+-+-+-+-+              -/            -/
     *
     *   N_S: Number of spatial layers minus 1
     *   Y: Each spatial layer's resolution present
     *   G: Picture Group description present
     *   N_G: Number of pictures in Picture Group
     *   TID: Temporal layer ID
     *   U: Switching up point
     *   R: Number of P_DIFF fields
     */
    if (has_ss_data) {
        int n_s, y, g, i;
        if (len < 1) {
            av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
            return AVERROR_INVALIDDATA;
        }
        n_s = buf[0] >> 5;
        y = !!(buf[0] & 0x10);
        g = !!(buf[0] & 0x08);
        buf++;
        len--;
        if (n_s > 0) {
            avpriv_report_missing_feature(ctx, "VP9 scalability structure with multiple layers");
            return AVERROR_PATCHWELCOME;
        }
        if (y) {
            if (len < 4 * (n_s + 1)) {
                av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
                return AVERROR_INVALIDDATA;
            }
            for (i = 0; i < n_s + 1; i++) {
                av_unused int w, h;
                w = AV_RB16(buf);
                h = AV_RB16(buf + 2);
                buf += 4;
                len -= 4;
            }
        }
        if (g) {
            int n_g;
            if (len < 1) {
                av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
                return AVERROR_INVALIDDATA;
            }
            n_g = buf[0];
            buf++;
            len--;
            for (i = 0; i < n_g; i++) {
                av_unused int t, u, r, j;
                if (len < 1) {
                    av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
                    return AVERROR_INVALIDDATA;
                }
                t = buf[0] >> 5;
                u = !!(buf[0] & 0x10);
                r = (buf[0] >> 2) & 0x03;
                buf++;
                len--;
                if (len < r) {
                    av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
                    return AVERROR_INVALIDDATA;
                }
                for (j = 0; j < r; j++) {
                    av_unused int p_diff = buf[0];
                    buf++;
                    len--;
                }
            }
        }
    }

    /* sanity check: 1 byte payload as minimum */
    if (len < 1) {
        av_log(ctx, AV_LOG_ERROR, "Too short RTP/VP9 packet\n");
        return AVERROR_INVALIDDATA;
    }

    /* start frame buffering with new dynamic buffer */
    if (!rtp_vp9_ctx->buf) {
        /* sanity check: a new frame should have started */
        if (first_fragment) {
            res = avio_open_dyn_buf(&rtp_vp9_ctx->buf);
            if (res < 0)
                return res;
            /* update the timestamp in the frame packet with the one from the RTP packet */
            rtp_vp9_ctx->timestamp = *timestamp;
        } else {
            /* frame not started yet, need more packets */
            return AVERROR(EAGAIN);
        }
    }

    /* write the fragment to the dyn. buffer */
    avio_write(rtp_vp9_ctx->buf, buf, len);

    /* do we need more fragments? */
    if (!last_fragment)
        return AVERROR(EAGAIN);

    /* close frame buffering and create resulting A/V packet */
    res = ff_rtp_finalize_packet(pkt, &rtp_vp9_ctx->buf, st->index);
    if (res < 0)
        return res;

    return 0;
}

static void vp9_close_context(PayloadContext *vp9)
{
    ffio_free_dyn_buf(&vp9->buf);
}

const RTPDynamicProtocolHandler ff_vp9_dynamic_handler = {
    .enc_name         = "VP9",
    .codec_type       = AVMEDIA_TYPE_VIDEO,
    .codec_id         = AV_CODEC_ID_VP9,
    .priv_data_size   = sizeof(PayloadContext),
    .init             = vp9_init,
    .close            = vp9_close_context,
    .parse_packet     = vp9_handle_packet
};