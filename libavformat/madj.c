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
#define MADJ_ID_VERSION 1
#define MADJ_ID_TAG 0x4D41444A /* MADJ */

#define MADJ_CODEC_VIDEO 0
#define MADJ_CODEC_AUDIO 1

#define MADJ_PARAM_STR 0
#define MADJ_PARAM_I64 1

/*** Generic ******************************************************************/
typedef struct {
    // Frame info
    uint64_t num_frames;
    uint64_t num_subframes;
    uint64_t data_offset;
    uint32_t rate_n;
    uint32_t rate_d;

    // Codec info
    uint32_t codec_type;
    uint32_t codec_id;
    uint32_t param_num;
    AVDictionary* param;
    
    // Index
    uint8_t* index;
} MadjTrack;

static int madj_read_string(AVIOContext *pb, char** str)
{
    uint16_t str_len = avio_rb16(pb);
    *str = av_malloc(str_len + 1);
    if (!(*str))
        return AVERROR(ENOMEM);
    if (avio_read(pb, (uint8_t*) *str, str_len) != str_len) {
        av_free(*str);
        return AVERROR(EIO);
    }
    str[str_len] = '\0';
    return 0;
}

static AVDictionaryEntry* madj_dict_find(AVDictionary* m, const char* key)
{
    return av_dict_get(m, key, NULL, AV_DICT_MATCH_CASE);
}

static void madj_dict_get_int(AVDictionary* m, const char* key, int* value)
{
    long v;
    AVDictionaryEntry* entry = madj_dict_find(m, key);
    if (!entry)
        return;
    v = strtol(entry->value, NULL, 0);
    if (v != LONG_MIN && v != LONG_MAX)
        *value = v;
}

static void madj_dict_get_int_nz(AVDictionary* m, const char* key, int* value)
{
    int v = 0;
    madj_dict_get_int(m, key, &v);
    if (v != 0)
        *value = v;
}

/*** Demuxer ******************************************************************/
typedef struct {
    // Context
    AVFormatContext *ctx;
    
    // Tracks
    uint32_t track_num;
    MadjTrack* track;
} MadjDemuxContext;

static int madj_probe(AVProbeData *p);
static int madj_read_header(AVFormatContext *s);
static int madj_read_packet(AVFormatContext *s, AVPacket *pkt);
static int madj_read_close(AVFormatContext *s);
static int madj_read_seek(AVFormatContext *s, int stream_index,
                          int64_t timestamp, int flags);

static int madj_probe(AVProbeData *p)
{
    // Check tag
    if (AV_RB32(p->buf) != MADJ_ID_TAG)
        return 0;
    
    // Valid format
    return AVPROBE_SCORE_MAX;
}

static int madj_read_header(AVFormatContext *s)
{
    // Error
    int err = 0;
    
    // Context
    MadjDemuxContext *madj = s->priv_data;
    memset(madj, 0, sizeof(MadjDemuxContext));
    madj->ctx = s;
    
    // Tag
    if (avio_rb32(madj->ctx->pb) != MADJ_ID_TAG)
        return AVERROR_INVALIDDATA;
    
    // Version
    if (avio_rb32(madj->ctx->pb) > MADJ_ID_VERSION)
        return AVERROR_PATCHWELCOME;
    
    // Header
    madj->track_num = avio_rb32(madj->ctx->pb);
    madj->track = av_mallocz(madj->track_num * sizeof(MadjTrack));
    for (uint32_t i = 0; i < madj->track_num; i++) {
        // Track
        MadjTrack *track = &madj->track[i];
        
        // Frame info
        track->num_frames = avio_rb64(madj->ctx->pb);
        track->num_subframes = avio_rb64(madj->ctx->pb);
        track->data_offset = avio_rb64(madj->ctx->pb);
        track->rate_n = avio_rb32(madj->ctx->pb);
        track->rate_d = avio_rb32(madj->ctx->pb);
        
        // Codec info
        track->codec_type = avio_rb32(madj->ctx->pb);
        track->codec_id = avio_rb32(madj->ctx->pb);
        track->param_num = avio_rb32(madj->ctx->pb);
        track->param = NULL;
        for (uint32_t j = 0; j < track->param_num; j++) {
            char* key = NULL;
            char* value = NULL;
            err = madj_read_string(madj->ctx->pb, &key);
            if (err != 0 || !key)
                goto error;
            err = madj_read_string(madj->ctx->pb, &value);
            if (err != 0 || !key)
                goto error;
            av_dict_set(&track->param, key, value, AV_DICT_DONT_STRDUP_KEY | AV_DICT_DONT_STRDUP_VAL);
        }
        
        // Index
        track->index = av_malloc(track->num_frames * 8);
        if (!track->index) {
            err = AVERROR(ENOMEM);
            goto error;
        }
        if (avio_read(madj->ctx->pb, (uint8_t*) track->index,
                      track->num_frames * 8) != track->num_frames * 8)
        {
            err = AVERROR(EIO);
            goto error;
        }
    }
    
    // Tracks
    for (uint32_t i = 0; i < madj->track_num; i++) {
        // Track
        MadjTrack* track = &madj->track[i];
        
        // Create stream
        AVStream* stream = avformat_new_stream(madj->ctx, NULL);
        if (!stream) {
            err = AVERROR(ENOMEM);
            goto error;
        }
        
        // Private data
        stream->priv_data = track;
        
        // Generic properties
        stream->time_base.num = track->rate_n;
        stream->time_base.den = track->rate_d;
        stream->start_time = 0;
        stream->duration = track->num_frames * track->num_subframes;
        stream->nb_frames = track->num_frames * track->num_subframes;
        av_dict_copy(&stream->metadata, track->param, AV_DICT_MATCH_CASE);
        
        // Codec
        if (track->codec_type == MADJ_CODEC_VIDEO) {
            AVCodecContext* codec = stream->codec;
            codec->codec_type = AVMEDIA_TYPE_VIDEO;
            codec->codec_tag = track->codec_id;
            
            madj_dict_get_int_nz(track->param, "frame_width", &codec->width);
            madj_dict_get_int_nz(track->param, "frame_height", &codec->height);
            {
                int display_width = 0;
                int display_height = 0;
                madj_dict_get_int(track->param, "display_width", &display_width);
                madj_dict_get_int(track->param, "display_height", &display_height);
                if (display_width != 0 && display_height != 0) {
                    av_reduce(&stream->sample_aspect_ratio.num,
                              &stream->sample_aspect_ratio.den,
                              codec->height * display_width,
                              codec->width * display_height,
                              255);
                }
            }
            
        } else if (track->codec_type == MADJ_CODEC_AUDIO) {
            AVCodecContext* codec = stream->codec;
            codec->codec_type = AVMEDIA_TYPE_AUDIO;
            codec->codec_tag = track->codec_id;
            
            madj_dict_get_int_nz(track->param, "sample_rate", &codec->sample_rate);
            madj_dict_get_int_nz(track->param, "channles", &codec->channels);
            madj_dict_get_int_nz(track->param, "bit_depth", &codec->bits_per_coded_sample);
            
        } else {
            // Unknown codec type error
            err = AVERROR_INVALIDDATA;
            goto error;
        }
    }
    
    // Success
    return 0;
    
    // Error
    error:
        // Close and return error code
        madj_read_close(s);
        return err;
}

static int madj_read_packet(AVFormatContext *s, AVPacket *pkt)
{
    return 0;
}

static int madj_read_close(AVFormatContext *s)
{
    // Context
    MadjDemuxContext *madj = s->priv_data;
    
    // Free memory
    if (madj->track) {
        for (uint32_t i = 0; i < madj->track_num; i++) {
            if (madj->track[i].param)
                av_dict_free(&madj->track[i].param);
            if (madj->track[i].index)
                av_freep(&madj->track[i].index);
        }
        av_freep(&madj->track);
    }
    
    // Success
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

