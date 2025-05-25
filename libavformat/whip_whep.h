/*
 * WHIP/WHEP shared functions
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

#ifndef AVFORMAT_WHIP_WHEP_H
#define AVFORMAT_WHIP_WHEP_H

#include "internal.h"
#define SDP_MAX_SIZE 16384

/**
 * Init the rtc logger for the WHIP/WHEP.
 */
void ff_whip_whep_init_rtc_logger(void);

/**
 * Exchange and set the SDP for the WHIP/WHEP.
 *
 * @param[in]  s           The format context.
 * @param[in]  pc          The pc id created by libdatachannel.
 * @param[in]  token       The token to use for the WHIP/WHEP server.
 * @param[out] session_url The session url from Location header in response; must be freed using av_free().
 * @return 0 on success, a negative value on error.
 */
int ff_whip_whep_exchange_and_set_sdp(AVFormatContext *s, int pc, const char* token, char **session_url);

/**
 * Delete the session for the WHIP/WHEP.
 *
 * @param[in]  s           The format context.
 * @param[in]  token       The token to use for the WHIP/WHEP server.
 * @param[in]  session_url The resource url to delete.
 * @return 0 on success, a negative value on error.
 */
int ff_whip_whep_delete_session(AVFormatContext *s, const char *token, const char *session_url);

#endif /* AVFORMAT_WHIP_WHEP_H */