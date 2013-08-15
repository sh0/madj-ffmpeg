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
#include "libavutil/avstring.h"
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
    uint32_t size;
    uint64_t offset;
    uint8_t* data;
} MadjChunk;

typedef struct {
    // Frame info
    uint64_t num_frames;
    uint64_t num_subframes;
    uint64_t data_offset;
    AVRational rate;

    // Codec info
    uint32_t codec_type;
    uint32_t codec_id;
    uint32_t param_num;
    AVDictionary* param;
    
    // Index
    uint8_t* index;
    
    // Decoding
    double decode_rate;
    uint64_t decode_frame;
    
    // Encoding
    uint32_t encode_num;
    uint64_t encode_offset;
    MadjChunk** encode_data;
} MadjTrack;

static int madj_read_string(AVIOContext* pb, char** str)
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

static int madj_write_string(AVIOContext* pb, char* str)
{
    uint16_t str_len = strlen(str);
    avio_wb16(pb, str_len);
    avio_write(pb, str, str_len);
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

static void madj_dict_set_int(AVDictionary** pm, const char* key, int value)
{
    char* str = av_asprintf("%d", value);
    av_dict_set(pm, key, str, AV_DICT_DONT_STRDUP_VAL);
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
        track->rate.num = avio_rb32(madj->ctx->pb);
        track->rate.den = avio_rb32(madj->ctx->pb);
        
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
        
        // Playback
        track->decode_rate = av_q2d(track->rate);
        track->decode_frame = 0;
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
        stream->time_base.num = track->rate.num;
        stream->time_base.den = track->rate.den;
        stream->start_time = 0;
        stream->duration = track->num_frames * track->num_subframes;
        stream->nb_frames = track->num_frames * track->num_subframes;
        av_dict_copy(&stream->metadata, track->param, AV_DICT_MATCH_CASE);
        
        // Codec
        if (track->codec_type == MADJ_CODEC_VIDEO) {
            AVCodecContext* codec = stream->codec;
            codec->codec_type = AVMEDIA_TYPE_VIDEO;
            codec->codec_tag = track->codec_id;
            codec->codec_id = track->codec_id;
            
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
            codec->codec_id = track->codec_id;
            
            madj_dict_get_int_nz(track->param, "sample_rate", &codec->sample_rate);
            madj_dict_get_int_nz(track->param, "channels", &codec->channels);
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
    // Variables
    uint8_t* index = NULL;
    uint64_t offset = 0;
    uint64_t size = 0;
    
    // Context
    MadjDemuxContext *madj = s->priv_data;
    
    // Get stream with lowest play position
    MadjTrack* track = NULL;
    int track_id = 0;
    double track_time = 0;
    for (int i = 0; i < madj->track_num; i++) {
        if (madj->track[i].decode_frame < madj->track[i].num_frames) {
            double ct = madj->track[i].decode_rate;
            ct *= madj->track[i].decode_frame * madj->track[i].num_subframes;
            if (track == NULL || track_time > ct) {
                track = &madj->track[i];
                track_id = i;
                track_time = ct;
            }
        }
    }
    if (!track)
        return AVERROR_EOF;
    
    // Index data
    index = track->index + (track->decode_frame * 8);
    
    // Offset and size
    for (int i = 0; i < 3; i++) {
        size <<= 8;
        size |= index[i];
    }
    for (int i = 0; i < 5; i++) {
        offset <<= 8;
        offset |= index[i + 3];
    }
    offset += track->data_offset;
    
    // Packet info
    av_new_packet(pkt, size);
    pkt->pts = track->decode_frame;
    pkt->dts = track->decode_frame;
    pkt->stream_index = track_id;
    pkt->duration = track->num_subframes;
    pkt->pos = offset;
    
    // Read packet
    avio_seek(madj->ctx->pb, offset, SEEK_SET);
    avio_read(madj->ctx->pb, pkt->data, size);
    
    // Increase frame counter
    track->decode_frame++;
    
    // Success
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
    // Context
    MadjDemuxContext *madj = s->priv_data;
    
    // Time
    double ts = 0;
    
    // Mode selection
    if (stream_index < 0) {
        ts = (double)timestamp / (double)AV_TIME_BASE;
    } else {
        MadjTrack* track;
        if (stream_index >= madj->track_num)
            return -1;
        track = &madj->track[stream_index];
        ts = track->decode_rate * (double)timestamp + (track->decode_rate / 10.0);
    }
    
    // Seeks
    for (int i = 0; i < madj->track_num; i++) {
        MadjTrack* track = &madj->track[i];
        track->decode_frame = ts / track->decode_rate;
        track->decode_frame /= track->num_subframes;
    }

    // Success
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
    // Context
    const AVClass* class;
    AVFormatContext *ctx;
    
    // Tracks
    uint32_t track_num;
    MadjTrack* track;
} MadjMuxContext;

static int madj_write_header(AVFormatContext *s)
{
    // Context
    MadjMuxContext *madj = s->priv_data;
    memset(madj, 0, sizeof(MadjMuxContext));
    madj->ctx = s;
    
    // Populate tracks
    madj->track_num = madj->ctx->nb_streams;
    madj->track = av_mallocz(madj->track_num * sizeof(MadjTrack));
    for (int i = 0; i < madj->track_num; i++) {
        // Track and stream
        MadjTrack* track = &madj->track[i];
        AVStream* stream = madj->ctx->streams[i];
        AVCodecContext* codec = stream->codec;
        
        // Frame info
        track->rate = stream->time_base;
        track->num_subframes = 1;
        
        // Codec info
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            // Video stream
            track->codec_type = MADJ_CODEC_VIDEO;
            track->codec_id = codec->codec_id;
            
            madj_dict_set_int(&track->param, "frame_width", codec->width);
            madj_dict_set_int(&track->param, "frame_height", codec->height);
            {
                AVRational sar = stream->sample_aspect_ratio;
                int display_width = av_rescale(codec->width, sar.num, sar.den);
                int display_height = av_rescale(codec->height, sar.num, sar.den);
                madj_dict_set_int(&track->param, "display_width", display_width);
                madj_dict_set_int(&track->param, "display_height", display_height);
            }
            track->param_num = av_dict_count(track->param);
            
        } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            // Audio stream
            track->codec_type = MADJ_CODEC_AUDIO;
            track->codec_id = codec->codec_id;
            
            madj_dict_set_int(&track->param, "sample_rate", codec->sample_rate);
            madj_dict_set_int(&track->param, "channels", codec->channels);
            madj_dict_set_int(&track->param, "bit_depth", codec->bits_per_coded_sample);
            track->param_num = av_dict_count(track->param);
            
            track->num_subframes = codec->frame_size * codec->channels;
            
        } else {
            // Unsupported type
            return -1;
        }
    }
    
    // Success
    return 0;
}

static int madj_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    // Context
    MadjMuxContext* madj = s->priv_data;
    MadjTrack* track = &madj->track[pkt->stream_index];
    
    // Chunk
    MadjChunk* chunk = av_mallocz(sizeof(MadjChunk));
    chunk->offset = track->encode_offset;
    chunk->size = pkt->size;
    chunk->data = av_memdup(pkt->data, pkt->size);
    
    // Track
    track->encode_offset += chunk->size;
    av_dynarray_add(&track->encode_data, &track->encode_num, chunk);
    
    // Success
    return 0;
}

static int madj_write_trailer(AVFormatContext *s)
{
    // Context
    MadjMuxContext* madj = s->priv_data;
    
    // Offsets
    uint64_t header_offset = 0;
    uint64_t data_offset = 0;
    
    // Finalize tracks
    for (int i = 0; i < madj->track_num; i++) {
        // Track
        MadjTrack* track = &madj->track[i];
        
        // Frame info
        track->num_frames = track->encode_num;
        track->data_offset = data_offset;
        
        // Offsets
        header_offset += 4 * 8; // frame info
        header_offset += 3 * 4; // codec info
        {
            AVDictionaryEntry* t = NULL;
            while (t = av_dict_get(track->param, "", t, AV_DICT_IGNORE_SUFFIX)) {
                header_offset += 2 + strlen(t->key);
                header_offset += 2 + strlen(t->value);
            }
        }
        header_offset += track->num_frames * 8; // index
        data_offset += track->encode_offset;
        
        // Index
        track->index = av_malloc(track->num_frames * 8);
        for (int j = 0; j < track->num_frames; j++) {
            uint8_t* index = track->index + (j * 8);
            uint32_t size = track->encode_data[j]->size;
            uint64_t offset = track->encode_data[j]->offset;
            for (int k = 0; k < 3; k++) {
                index[2 - k] = (size & 0xff);
                size >>= 8;
            }
            for (int k = 0; k < 5; k++) {
                index[7 - k] = (offset & 0xff);
                offset >>= 8;
            }
        }
    }
    
    // Fix offsets
    for (int i = 0; i < madj->track_num; i++)
        madj->track[i].data_offset += header_offset;
    
    // Write tag and version
    avio_wb32(madj->ctx->pb, MADJ_ID_TAG);
    avio_wb32(madj->ctx->pb, MADJ_ID_VERSION);
    
    // Write track info
    avio_wb32(madj->ctx->pb, madj->track_num);
    for (int i = 0; i < madj->track_num; i++) {
        // Track
        MadjTrack* track = &madj->track[i];
        
        // Frame info
        avio_wb64(madj->ctx->pb, track->num_frames);
        avio_wb64(madj->ctx->pb, track->num_subframes);
        avio_wb64(madj->ctx->pb, track->data_offset);
        avio_wb32(madj->ctx->pb, track->rate.num);
        avio_wb32(madj->ctx->pb, track->rate.den);

        // Codec info
        avio_wb32(madj->ctx->pb, track->codec_type);
        avio_wb32(madj->ctx->pb, track->codec_id);
        avio_wb32(madj->ctx->pb, track->param_num);
        {
            AVDictionaryEntry* t = NULL;
            while (t = av_dict_get(track->param, "", t, AV_DICT_IGNORE_SUFFIX)) {
                madj_write_string(madj->ctx->pb, t->key);
                madj_write_string(madj->ctx->pb, t->value);
            }
        }
        
        // Index
        avio_write(madj->ctx->pb, track->index, track->num_frames * 8);
    }
    
    // Write data
    for (int i = 0; i < madj->track_num; i++) {
        // Track
        MadjTrack* track = &madj->track[i];
        
        // Chunks
        for (int j = 0; j < track->encode_num; j++) {
            MadjChunk* chunk = track->encode_data[j];
            avio_write(madj->ctx->pb, chunk->data, chunk->size);
        }
    }
    
    // Success
    return 0;
}

static int madj_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    // Allow all video and audio codeces for now
    enum AVMediaType type = avcodec_get_type(codec_id);
    if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
        return 1;
    
    // Failure
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

