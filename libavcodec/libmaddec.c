/*
 * MP3 decoder using libmad
 * Copyright (c) 2022 David Fletcher
 *
 * This file is part of FFmpeg.
 *
 * Fixed by Martin Eesmaa (2025)
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

#include <mad.h>

#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#define MAD_BUFSIZE (32 * 1024)
#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct libmad_context {
    uint8_t input_buffer[MAD_BUFSIZE+MAD_BUFFER_GUARD];
    struct mad_synth  synth; 
    struct mad_stream stream;
    struct mad_frame  frame;
    struct mad_header header;
    int got_header;
} libmad_context;

/* utility to scale and round samples to 16 bits */
static inline signed int mad_scale(mad_fixed_t sample)
{
    /* round */
    sample += (1L << (MAD_F_FRACBITS - 16));

    /* clip */
    if (sample >= MAD_F_ONE)
        sample = MAD_F_ONE - 1;
    else if (sample < -MAD_F_ONE)
        sample = -MAD_F_ONE;

    /* quantize */
    return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static av_cold int libmad_decode_init(AVCodecContext *avc)
{
    libmad_context *mad = avc->priv_data;

    mad_synth_init  (&mad->synth);
    mad_stream_init (&mad->stream);
    mad_frame_init  (&mad->frame);
    mad->got_header = 0;

    return 0;
}

static av_cold int libmad_decode_close(AVCodecContext *avc)
{
    libmad_context *mad = avc->priv_data;

    mad_synth_finish(&mad->synth);
    mad_frame_finish(&mad->frame);
    mad_stream_finish(&mad->stream);

    return 0;
}

static int libmad_decode_frame(AVCodecContext *avc, void *data,
                               int *got_frame_ptr, AVPacket *pkt)
{
    AVFrame *frame = data;
    libmad_context *mad = avc->priv_data;
    struct mad_pcm *pcm;
    mad_fixed_t const *left_ch;
    mad_fixed_t const *right_ch;
    int16_t *output;
    int nsamples;
    int nchannels;
    size_t bytes_read = 0;
    size_t remaining = 0;

    remaining = mad->stream.bufend - mad->stream.next_frame;
    if (remaining > 0)
        memmove(mad->input_buffer, mad->stream.next_frame, remaining);
    bytes_read = MIN(pkt->size, MAD_BUFSIZE - remaining);
    memcpy(mad->input_buffer + remaining, pkt->data, bytes_read);

    if (bytes_read == 0) {
        *got_frame_ptr = 0;
        return 0;
    }

    mad_stream_buffer(&mad->stream, mad->input_buffer, remaining + bytes_read);
    mad->stream.error = 0;

    if (!mad->got_header) {
        mad_header_decode(&mad->header, &mad->stream);
        mad->got_header = 1;
        avc->frame_size = 32 * (mad->header.layer == MAD_LAYER_I ? 12 :
                                ((mad->header.layer == MAD_LAYER_III &&
                                  (mad->header.flags & MAD_FLAG_LSF_EXT)) ? 18 : 36));
        avc->sample_fmt = AV_SAMPLE_FMT_S16;
        if (mad->header.mode == MAD_MODE_SINGLE_CHANNEL) {
            av_channel_layout_default(&avc->ch_layout, 1);
        } else {
            av_channel_layout_default(&avc->ch_layout, 2);
        }
    }

    av_channel_layout_copy(&frame->ch_layout, &avc->ch_layout);
    frame->format = avc->sample_fmt;
    frame->nb_samples = avc->frame_size;

    int ret = ff_get_buffer(avc, frame, 0);
    if (ret < 0)
        return ret;

    if (mad_frame_decode(&mad->frame, &mad->stream) == -1) {
        *got_frame_ptr = 0;
        return mad->stream.bufend - mad->stream.next_frame;
    }

    mad_synth_frame(&mad->synth, &mad->frame);

    pcm = &mad->synth.pcm;
    output = (int16_t *)frame->data[0];
    nsamples = pcm->length;
    nchannels = pcm->channels;
    left_ch = pcm->samples[0];
    right_ch = pcm->samples[1];
    while (nsamples--) {
        *output++ = mad_scale(*(left_ch++));
        if (nchannels == 2) {
            *output++ = mad_scale(*(right_ch++));
        }
        //Players should recognise mono and play through both channels
        //Writing the same thing to both left and right channels here causes
        //memory issues as it creates double the number of samples allocated.
    }

    *got_frame_ptr = 1;

    return mad->stream.bufend - mad->stream.next_frame;
}

FFCodec ff_libmad_decoder = {
    .p.name           = "libmad",
    CODEC_LONG_NAME("libmad MP3 decoder"),
    .p.type           = AVMEDIA_TYPE_AUDIO,
    .p.id             = AV_CODEC_ID_MP3,
    .p.sample_fmts    = (const enum AVSampleFormat[]) { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE },
    .p.capabilities   = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_CHANNEL_CONF,
    .priv_data_size   = sizeof(libmad_context),
    .init             = libmad_decode_init,
    .close            = libmad_decode_close,
    FF_CODEC_DECODE_CB(libmad_decode_frame),
    .p.wrapper_name   = "libmad",
};
