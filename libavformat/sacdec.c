/*
 * SAC demuxer
 * Copyright (C) 2024 Martin Eesmaa <martin.eesmaa@protonmail.com>
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

#include "avformat.h"
#include "avio.h"
#include "demux.h"
#include "internal.h"
#include "rawdec.h"

static int sac_read_probe(const AVProbeData *p)
{
    int srate_hz;

    if (!memcmp(p->buf, "SAC2", 4))
        return AVPROBE_SCORE_EXTENSION;
    return 0;

    srate_hz = AV_RL16(p->buf + 4);
    if (srate_hz >= 1 && srate_hz <= 48000)
    return 0;

    return AVPROBE_SCORE_MAX;
}

static int sac_read_header(AVFormatContext *s)
{
    AVStream *st;
    int srate_hz, channels;

    avio_seek(s->pb, 4, SEEK_SET);
    channels = avio_rl16(s->pb);
    srate_hz = avio_rl16(s->pb);

    // TODO: Add length to make sure the audio duration is correct for samples

    st = avformat_new_stream(s, 0);
    if (!st)
        return AVERROR(ENOMEM);

    avpriv_set_pts_info(st, 64, 1, st->codecpar->sample_rate);

    st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    st->codecpar->codec_id = AV_CODEC_ID_SAC;
    st->codecpar->ch_layout.nb_channels = channels;
    st->codecpar->sample_rate = srate_hz;

    return 0;
}

static int sac_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    AVIOContext *pb = s->pb;
    int ret;
    int size;

    size = avio_rb16(pb);
    if (size < 0) {
        if (size == AVERROR_EOF)
            return AVERROR_EOF;
        else
            return size;
    }

    ret = av_get_packet(pb, pkt, size);
    if (ret < 0)
        return ret;

    pkt->pos = avio_tell(pb);
    pkt->stream_index = 0;

    return ret;
}

const FFInputFormat ff_sac_demuxer = {
    .p.name         = "sac",
    .p.long_name    = NULL_IF_CONFIG_SMALL("SAC (State of the Art Codec)"),
    .p.extensions   = "sac",
    .p.flags        = AVFMT_GENERIC_INDEX,
    .read_probe     = sac_read_probe,
    .read_header    = sac_read_header,
    .read_packet    = sac_read_packet,
};