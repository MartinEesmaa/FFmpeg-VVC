/*
 * LC3 muxer
 * Copyright (C) 2024  Antoine Soulier <asoulier@google.com>
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

/**
 * @file
 * Based on the file format specified by :
 *
 * - Bluetooth SIG - Low Complexity Communication Codec Test Suite
 *   https://www.bluetooth.org/docman/handlers/downloaddoc.ashx?doc_id=502301
 *   3.2.8.2 Reference LC3 Codec Bitstream Format
 *
 * - ETSI TI 103 634 V1.4.1 - Low Complexity Communication Codec plus
 *   https://www.etsi.org/deliver/etsi_ts/103600_103699/103634/01.04.01_60/ts_103634v010401p.pdf
 *   LC3plus conformance script package
 */

#include "libavutil/intreadwrite.h"

#include "avformat.h"
#include "avio.h"
#include "mux.h"
#include "internal.h"

static av_cold int lc3_init(AVFormatContext *s)
{
    if (s->nb_streams != 1) {
        av_log(s, AV_LOG_ERROR, "This muxer only supports a single stream.\n");
        return AVERROR(EINVAL);
    }

    return 0;
}

static int lc3_write_header(AVFormatContext *s)
{
    AVStream *st = s->streams[0];
    int channels = st->codecpar->ch_layout.nb_channels;
    int srate_hz = st->codecpar->sample_rate;
    int bit_rate = st->codecpar->bit_rate;
    int frame_us, ep_mode, hr_mode;
    uint32_t nb_samples = av_rescale_q(
        st->duration, st->time_base, (AVRational){ 1, srate_hz });

    if (st->codecpar->extradata_size < 6)
        return AVERROR_INVALIDDATA;

    frame_us = AV_RL16(st->codecpar->extradata + 0) * 10;
    ep_mode = AV_RL16(st->codecpar->extradata + 2) != 0;
    hr_mode = AV_RL16(st->codecpar->extradata + 4) != 0;

    if (srate_hz !=  8000 && srate_hz != 16000 && srate_hz != 24000 &&
        srate_hz != 32000 && srate_hz != 48000 && srate_hz != 96000) {
        av_log(s, AV_LOG_ERROR, "Incompatible LC3 sample rate: %d Hz.\n",
               srate_hz);
        return AVERROR_INVALIDDATA;
    }

    if (frame_us != 2500 && frame_us !=  5000 &&
        frame_us != 7500 && frame_us != 10000) {
        av_log(s, AV_LOG_ERROR, "Incompatible LC3 frame duration: %.1f ms.\n",
               frame_us / 1000.f);
        return AVERROR_INVALIDDATA;
    }

    avio_wb16(s->pb, 0x1ccc);
    avio_wl16(s->pb, (9 + hr_mode) * sizeof(uint16_t));
    avio_wl16(s->pb, srate_hz / 100);
    avio_wl16(s->pb, bit_rate / 100);
    avio_wl16(s->pb, channels);
    avio_wl16(s->pb, frame_us / 10);
    avio_wl16(s->pb, ep_mode);
    avio_wl32(s->pb, nb_samples);
    if (hr_mode)
        avio_wl16(s->pb, hr_mode);

    return 0;
}

static int lc3_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    avio_wl16(s->pb, pkt->size);
    avio_write(s->pb, pkt->data, pkt->size);
    return 0;
}

const FFOutputFormat ff_lc3_muxer = {
    .p.name        = "lc3",
    .p.long_name   = NULL_IF_CONFIG_SMALL("LC3 (Low Complexity Communication Codec)"),
    .p.extensions  = "lc3",
    .p.audio_codec = AV_CODEC_ID_LC3,
    .p.video_codec = AV_CODEC_ID_NONE,
    .p.flags       = AVFMT_NOTIMESTAMPS,
    .init          = lc3_init,
    .write_header  = lc3_write_header,
    .write_packet  = lc3_write_packet,
};
