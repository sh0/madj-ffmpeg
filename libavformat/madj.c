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
#include "libavutil/pixdesc.h"

/*** Macros *******************************************************************/
#define MADJ_ID_VERSION 1
#define MADJ_ID_TAG 0x4D41444A /* MADJ */

#define MADJ_CODEC_VIDEO 0
#define MADJ_CODEC_AUDIO 1
#define MADJ_HEADER_SIZE_VIDEO 20
#define MADJ_HEADER_SIZE_AUDIO 12

/*** Generic ******************************************************************/
typedef struct {
    uint32_t size;
    uint64_t offset;
    uint8_t* data;
} MadjChunk;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t pixfmt;
} MadjVideo;

typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_coded_sample;
} MadjAudio;

typedef struct {
    // Stream
    AVStream* stream;

    // Frame info
    uint64_t num_frames;
    uint64_t num_subframes;
    uint64_t data_offset;
    AVRational rate;

    // Codec info
    uint32_t codec_type;
    uint32_t codec_id;
    union {
        MadjVideo codec_video;
        MadjAudio codec_audio;
    };
    
    // Index
    uint64_t* index;
    
    // Decoding
    double decode_rate;
    uint64_t decode_frame;
    
    // Encoding
    uint32_t encode_num;
    uint64_t encode_offset;
    MadjChunk** encode_data;
} MadjTrack;

/*** Demuxer ******************************************************************/
typedef struct {
    // Context
    AVFormatContext* ctx;
    
    // Tracks
    uint32_t track_num;
    MadjTrack* track;
} MadjDemuxContext;

static int madj_probe(AVProbeData* p);
static int madj_read_header(AVFormatContext* s);
static int madj_read_packet(AVFormatContext* s, AVPacket* pkt);
static int madj_read_close(AVFormatContext* s);
static int madj_read_seek(AVFormatContext* s, int stream_index,
                          int64_t timestamp, int flags);

static int madj_probe(AVProbeData* p)
{
    // Check tag
    if (AV_RB32(p->buf) != MADJ_ID_TAG)
        return 0;
    
    // Valid format
    return AVPROBE_SCORE_MAX;
}

static int madj_read_header(AVFormatContext* s)
{
    // Error
    int err = 0;
    
    // Context
    MadjDemuxContext* madj = s->priv_data;
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

        // Codec specific data
        if (track->codec_type == MADJ_CODEC_VIDEO) {
            // Video track
            MadjVideo* video = &track->codec_video;
            video->width = avio_rb32(madj->ctx->pb);
            video->height = avio_rb32(madj->ctx->pb);
            video->display_width = avio_rb32(madj->ctx->pb);
            video->display_height = avio_rb32(madj->ctx->pb);
            video->pixfmt = avio_rb32(madj->ctx->pb);

        } else if (track->codec_type == MADJ_CODEC_AUDIO) {
            // Audio track
            MadjAudio* audio = &track->codec_audio;
            audio->sample_rate = avio_rb32(madj->ctx->pb);
            audio->channels = avio_rb32(madj->ctx->pb);
            audio->bits_per_coded_sample = avio_rb32(madj->ctx->pb);

        } else {
            // Unsupported track type
            return AVERROR_INVALIDDATA;
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
        track->stream = avformat_new_stream(madj->ctx, NULL);
        if (!track->stream) {
            err = AVERROR(ENOMEM);
            goto error;
        }
        
        // Private data
        track->stream->priv_data = track;
        track->stream->codec->extradata = NULL;
        
        // Generic properties
        avpriv_set_pts_info(track->stream, 64, track->rate.num, track->rate.den);
        track->stream->start_time = 0;
        track->stream->duration = track->num_frames * track->num_subframes;
        track->stream->nb_frames = track->num_frames * track->num_subframes;

        /*
        //track->stream->time_base.num = 1;
        //track->stream->time_base.den = 1000;
        
        //track->stream->codec->time_base = track->rate;
        
        //AVRational tr;
        //tr.num = 1;
        //tr.den = 1;
        //av_codec_set_pkt_timebase(track->stream->codec, tr);
        track->stream->codec->ticks_per_frame = 1;
        track->stream->avg_frame_rate = track->rate;
        track->stream->avg_frame_rate.num *= 500;
        */
        
        // Codec
        if (track->codec_type == MADJ_CODEC_VIDEO) {
            // Video codec
            AVCodecContext* codec = track->stream->codec;
            codec->codec_type = AVMEDIA_TYPE_VIDEO;
            //codec->codec_tag = track->codec_id;
            codec->codec_id = track->codec_id;

            codec->width = track->codec_video.width;
            codec->height = track->codec_video.height;
            if (track->codec_video.display_width != 0 &&
                track->codec_video.display_height != 0
            ) {
                av_reduce(&track->stream->sample_aspect_ratio.num,
                          &track->stream->sample_aspect_ratio.den,
                          codec->height * track->codec_video.display_width,
                          codec->width * track->codec_video.display_height,
                          255);
            }

            if (track->codec_video.pixfmt != 0) {
                for (int i = AV_PIX_FMT_NONE; i < AV_PIX_FMT_NB; i++) {
                    if (avcodec_pix_fmt_to_codec_tag(i) == track->codec_video.pixfmt) {
                        codec->pix_fmt = i;
                        break;
                    }
                }
            }
            
        } else if (track->codec_type == MADJ_CODEC_AUDIO) {
            // Audio codec
            AVCodecContext* codec = track->stream->codec;
            codec->codec_type = AVMEDIA_TYPE_AUDIO;
            //codec->codec_tag = track->codec_id;
            codec->codec_id = track->codec_id;

            codec->sample_rate = track->codec_audio.sample_rate;
            codec->channels = track->codec_audio.channels;
            codec->bits_per_coded_sample = track->codec_audio.bits_per_coded_sample;
            if (codec->channels > 0)
                codec->frame_size = track->num_subframes / codec->channels;
            
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
    index = (uint8_t*) &track->index[track->decode_frame];
    
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
    /*
    av_new_packet(pkt, size);
    pkt->pts = track->decode_frame;
    pkt->dts = track->decode_frame;
    pkt->stream_index = track_id;
    pkt->duration = track->num_subframes;
    pkt->pos = offset;
    */
    
    // Read packet
    avio_seek(madj->ctx->pb, offset, SEEK_SET);
    av_get_packet(madj->ctx->pb, pkt, size);
    //avio_read(madj->ctx->pb, pkt->data, size);
    
    // Packet info
    pkt->pts = track->decode_frame;
    pkt->dts = track->decode_frame;
    pkt->stream_index = track_id;
    pkt->duration = track->num_subframes;

    // Debug
    /*
    FILE* fs = fopen("temp.jpg", "wb");
    fwrite(pkt->data, pkt->size, 1, fs);
    fclose(fs);
    exit(0);
    */
    
    // Increase frame counter
    track->decode_frame++;
    
    // Success
    return 0;
}

static int madj_read_close(AVFormatContext *s)
{
    // Context
    MadjDemuxContext *madj = s->priv_data;
    if (!madj)
        return 0;
    
    // Free memory
    if (madj->track) {
        for (uint32_t i = 0; i < madj->track_num; i++) {
            madj->track[i].stream->priv_data = NULL;
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
    
    // Debug
    av_log(s, AV_LOG_ERROR, "madj_read_seek\n");
    
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
        track->rate.num = codec->time_base.num;
        track->rate.den = codec->time_base.den;
        track->num_subframes = 1;
        
        // Codec info
        if (codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            // Video stream
            track->codec_type = MADJ_CODEC_VIDEO;
            track->codec_id = codec->codec_id;
            
            track->codec_video.width = codec->width;
            track->codec_video.height = codec->height;
            {
                AVRational sar = stream->sample_aspect_ratio;
                track->codec_video.display_width = av_rescale(codec->width, sar.num, sar.den);
                track->codec_video.display_height = av_rescale(codec->height, sar.num, sar.den);
            }
            track->codec_video.pixfmt = avcodec_pix_fmt_to_codec_tag(codec->pix_fmt);
            
        } else if (codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            // Audio stream
            track->codec_type = MADJ_CODEC_AUDIO;
            track->codec_id = codec->codec_id;
            
            track->codec_audio.sample_rate = codec->sample_rate;
            track->codec_audio.channels = codec->channels;
            track->codec_audio.bits_per_coded_sample = codec->bits_per_coded_sample;
            
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
    uint64_t header_offset = 4 + 4 + 4;
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
        header_offset += 2 * 4; // codec info
        if (track->codec_type == MADJ_CODEC_VIDEO) {
            header_offset += MADJ_HEADER_SIZE_VIDEO; // video header
        } else if (track->codec_type == MADJ_CODEC_AUDIO) {
            header_offset += MADJ_HEADER_SIZE_AUDIO; // audio header
        }
        header_offset += track->num_frames * 8; // index
        data_offset += track->encode_offset;
        
        // Index
        track->index = av_malloc(track->num_frames * 8);
        for (int j = 0; j < track->num_frames; j++) {
            uint8_t* index = (uint8_t*) &track->index[j];
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
        if (track->codec_type == MADJ_CODEC_VIDEO) {
            avio_wb32(madj->ctx->pb, track->codec_video.width);
            avio_wb32(madj->ctx->pb, track->codec_video.height);
            avio_wb32(madj->ctx->pb, track->codec_video.display_width);
            avio_wb32(madj->ctx->pb, track->codec_video.display_height);
            avio_wb32(madj->ctx->pb, track->codec_video.pixfmt);
        } else if (track->codec_type == MADJ_CODEC_AUDIO) {
            avio_wb32(madj->ctx->pb, track->codec_audio.sample_rate);
            avio_wb32(madj->ctx->pb, track->codec_audio.channels);
            avio_wb32(madj->ctx->pb, track->codec_audio.bits_per_coded_sample);
        }
        
        // Index
        avio_write(madj->ctx->pb, (uint8_t*) track->index, track->num_frames * 8);
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

#define MADJ_VIDEO_CODEC_ID AV_CODEC_ID_MJPEG
#define MADJ_AUDIO_CODEC_ID AV_CODEC_ID_MP3
//AV_CODEC_ID_PCM_S16BE

static int madj_query_codec(enum AVCodecID codec_id, int std_compliance)
{
    // Allow only PCM and MJPEG formats
    if (codec_id == MADJ_AUDIO_CODEC_ID || codec_id == MADJ_VIDEO_CODEC_ID)
        return 1;
    
    // Failure
    return 0;
}

/*
static const AVCodecTag additional_audio_tags[] = {
    { AV_CODEC_ID_EAC3,      0XFFFFFFFF },
    { AV_CODEC_ID_PCM_S16BE, 0xFFFFFFFF },
    { AV_CODEC_ID_PCM_S24BE, 0xFFFFFFFF },
    { AV_CODEC_ID_PCM_S32BE, 0xFFFFFFFF },
    { AV_CODEC_ID_NONE,      0xFFFFFFFF }
};
*/

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
    .audio_codec       = MADJ_AUDIO_CODEC_ID,
    .video_codec       = MADJ_VIDEO_CODEC_ID,
    .write_header      = madj_write_header,
    .write_packet      = madj_write_packet,
    .write_trailer     = madj_write_trailer,
    .flags             = AVFMT_GLOBALHEADER | AVFMT_TS_NONSTRICT,
    /*
    .codec_tag         = (const AVCodecTag* const []){
        additional_audio_tags, 0
    },
    */
    .query_codec       = madj_query_codec,
    .priv_class        = &madj_class,
};

