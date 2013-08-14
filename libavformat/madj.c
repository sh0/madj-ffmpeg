/*
 * MADJ files muxer and demuxer
 * Copyright (c) 2013 sh0
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

#include "avformat.h"
#include "internal.h"
#include "avio_internal.h"

#include "libavutil/opt.h"
#include "libavutil/intreadwrite.h"

/*** Macros *******************************************************************/
#define MADJ_VERSION 1
#define MADJ_ID_HEADER 0x4D41444A /* 'MADJ' */

/*** Demuxer ******************************************************************/
typedef struct {
    AVFormatContext *ctx;
} MadjDemuxContext;

static int madj_probe(AVProbeData *p)
{
    if (AV_RB32(p->buf) != MADJ_ID_HEADER)
        return 0;
    return AVPROBE_SCORE_MAX;
}

static int madj_read_header(AVFormatContext *s)
{
    return 0;
}

static int madj_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return 0;
}

static int madj_read_close(AVFormatContext *s)
{
    return 0;
}

static int madj_read_seek(AVFormatContext *s, int stream_index,
                          int64_t timestamp, int flags)
{
    return 0;
}

AVInputFormat ff_madj_demuxer = {
    .name           = "madj",
    .long_name      = NULL_IF_CONFIG_SMALL("MADJ"),
    .priv_data_size = sizeof(MadjDemuxContext),
    .read_probe     = madj_probe,
    .read_header    = madj_read_header,
    .read_packet    = madj_read_packet,
    .read_close     = madj_read_close,
    .read_seek      = madj_read_seek,
};

/*** Muxer ********************************************************************/
typedef struct MadjMuxContext {
    const AVClass *class;
    AVIOContext *dyn_bc;
} MadjMuxContext;

static int madj_write_header(AVFormatContext *s)
{
    return 0;
}

static int madj_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    return 0;
}

static int madj_write_trailer(AVFormatContext *s)
{
    return 0;
}

static int madj_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    return 0;
}

static const AVCodecTag additional_audio_tags[] = {
    { AV_CODEC_ID_EAC3,      0XFFFFFFFF },
    { AV_CODEC_ID_PCM_S16BE, 0xFFFFFFFF },
    { AV_CODEC_ID_PCM_S24BE, 0xFFFFFFFF },
    { AV_CODEC_ID_PCM_S32BE, 0xFFFFFFFF },
    { AV_CODEC_ID_NONE,      0xFFFFFFFF }
};

#define OFFSET(x) offsetof(MadjMuxContext, x)
#define FLAGS AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { NULL },
};

static const AVClass madj_class = {
    .class_name = "madj muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_madj_muxer = {
    .name              = "madj",
    .long_name         = NULL_IF_CONFIG_SMALL("MADJ"),
    .mime_type         = "video/x-madj",
    .extensions        = "mjv",
    .priv_data_size    = sizeof(MadjMuxContext),
    .audio_codec       = CONFIG_LIBVORBIS_ENCODER ?
                         AV_CODEC_ID_VORBIS : AV_CODEC_ID_AC3,
    .video_codec       = CONFIG_LIBX264_ENCODER ?
                         AV_CODEC_ID_H264 : AV_CODEC_ID_MPEG4,
    .write_header      = madj_write_header,
    .write_packet      = madj_write_packet,
    .write_trailer     = madj_write_trailer,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_TS_NONSTRICT,
    .codec_tag         = (const AVCodecTag* const []){
        additional_audio_tags, 0
    },
    .query_codec       = madj_query_codec,
    .priv_class        = &madj_class,
};

