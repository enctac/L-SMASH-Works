/*****************************************************************************
 * lwlibav_audio.c / lwlibav_audio.cpp
 *****************************************************************************
 * Copyright (C) 2012-2015 L-SMASH Works project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

#include "cpp_compat.h"

#ifdef __cplusplus
extern "C"
{
#endif  /* __cplusplus */
#include <libavformat/avformat.h>       /* Demuxer */
#include <libavcodec/avcodec.h>         /* Decoder */
#include <libavresample/avresample.h>   /* Resampler/Buffer */
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#ifdef __cplusplus
}
#endif  /* __cplusplus */

#include "utils.h"
#include "audio_output.h"
#include "resample.h"

#include "lwlibav_dec.h"
#include "lwlibav_audio.h"

int lwlibav_get_desired_audio_track
(
    const char                     *file_path,
    lwlibav_audio_decode_handler_t *adhp,
    int                             threads
)
{
    int error = adhp->stream_index < 0
             || adhp->frame_count == 0
             || lavf_open_file( &adhp->format, file_path, &adhp->lh );
    AVCodecContext *ctx = !error ? adhp->format->streams[ adhp->stream_index ]->codec : NULL;
    if( error || open_decoder( ctx, adhp->codec_id, threads ) )
    {
        if( adhp->index_entries )
            av_freep( &adhp->index_entries );
        if( adhp->frame_list )
            lw_freep( &adhp->frame_list );
        if( adhp->format )
        {
            lavf_close_file( &adhp->format );
            adhp->format = NULL;
        }
        return -1;
    }
    adhp->ctx = ctx;
    return 0;
}

uint64_t lwlibav_count_overall_pcm_samples
(
    lwlibav_audio_decode_handler_t *adhp,
    int                             output_sample_rate
)
{
    audio_frame_info_t *frame_list    = adhp->frame_list;
    int      current_sample_rate      = frame_list[1].sample_rate > 0 ? frame_list[1].sample_rate : adhp->ctx->sample_rate;
    uint32_t current_frame_length     = frame_list[1].length;
    uint64_t pcm_sample_count         = 0;
    uint64_t overall_pcm_sample_count = 0;
    for( uint32_t i = 1; i <= adhp->frame_count; i++ )
    {
        if( (current_sample_rate != frame_list[i].sample_rate && frame_list[i].sample_rate > 0)
         || current_frame_length != frame_list[i].length )
        {
            uint64_t resampled_sample_count = output_sample_rate == current_sample_rate || pcm_sample_count == 0
                                            ? pcm_sample_count
                                            : (pcm_sample_count * output_sample_rate - 1) / current_sample_rate + 1;
            overall_pcm_sample_count += resampled_sample_count;
            pcm_sample_count     = 0;
            current_sample_rate  = frame_list[i].sample_rate > 0 ? frame_list[i].sample_rate : adhp->ctx->sample_rate;
            current_frame_length = frame_list[i].length;
        }
        pcm_sample_count += frame_list[i].length;
    }
    current_sample_rate = frame_list[ adhp->frame_count ].sample_rate > 0
                        ? frame_list[ adhp->frame_count ].sample_rate
                        : adhp->ctx->sample_rate;
    if( pcm_sample_count )
        overall_pcm_sample_count += (pcm_sample_count * output_sample_rate - 1) / current_sample_rate + 1;
    return overall_pcm_sample_count;
}

static int find_start_audio_frame
(
    lwlibav_audio_decode_handler_t *adhp,
    int                             output_sample_rate,
    uint64_t                        start_frame_pos,
    uint64_t                       *start_offset
)
{
    audio_frame_info_t *frame_list = adhp->frame_list;
    uint32_t frame_number                    = 1;
    uint64_t current_frame_pos               = 0;
    uint64_t next_frame_pos                  = 0;
    int      current_sample_rate             = frame_list[frame_number].sample_rate > 0 ? frame_list[frame_number].sample_rate : adhp->ctx->sample_rate;
    int      current_frame_length            = frame_list[frame_number].length;
    uint64_t resampled_sample_count          = 0;   /* the number of accumulated PCM samples after resampling per sequence */
    uint64_t pcm_sample_count                = 0;   /* the number of accumulated PCM samples before resampling per sequence */
    uint64_t prior_sequences_resampled_count = 0;   /* the number of accumulated PCM samples of all prior sequences */
    do
    {
        current_frame_pos = next_frame_pos;
        if( (current_sample_rate != frame_list[frame_number].sample_rate && frame_list[frame_number].sample_rate > 0)
         || current_frame_length != frame_list[frame_number].length )
        {
            /* Encountered a new sequence. */
            prior_sequences_resampled_count += resampled_sample_count;
            pcm_sample_count = 0;
            current_sample_rate  = frame_list[frame_number].sample_rate > 0 ? frame_list[frame_number].sample_rate : adhp->ctx->sample_rate;
            current_frame_length = frame_list[frame_number].length;
        }
        pcm_sample_count += (uint64_t)current_frame_length;
        resampled_sample_count = output_sample_rate == current_sample_rate || pcm_sample_count == 0
                               ? pcm_sample_count
                               : (pcm_sample_count * output_sample_rate - 1) / current_sample_rate + 1;
        next_frame_pos = prior_sequences_resampled_count + resampled_sample_count;
        if( start_frame_pos < next_frame_pos )
            break;
        ++frame_number;
    } while( frame_number <= adhp->frame_count );
    *start_offset = start_frame_pos - current_frame_pos;
    if( *start_offset && current_sample_rate != output_sample_rate )
        *start_offset = (*start_offset * current_sample_rate - 1) / output_sample_rate + 1;
    if( frame_number > 1 )
    {
        /* Add pre-roll samples if needed.
         * The condition is irresponsible. Patches welcome. */
        enum AVCodecID codec_id = adhp->exh.entries[ frame_list[frame_number].extradata_index ].codec_id;
        const AVCodecDescriptor *desc = avcodec_descriptor_get( codec_id );
        if( (desc->props & AV_CODEC_PROP_LOSSY)
         && frame_list[frame_number].extradata_index == frame_list[frame_number - 1].extradata_index )
            *start_offset += (uint64_t)frame_list[--frame_number].length;
    }
    return frame_number;
}

static inline void make_null_packet
(
    AVPacket *pkt
)
{
    pkt->buf  = NULL;
    pkt->data = NULL;
    pkt->size = 0;
}

static void cleanup_av_bitstream_filter
(
    lwlibav_audio_decode_handler_t *adhp
)
{
    av_freep( &adhp->ctx->extradata );
    adhp->ctx->extradata_size = 0;
    av_free_packet( &adhp->alter_packet );
    av_bitstream_filter_close( adhp->bsf );
    adhp->bsf = NULL;
}

static void make_decodable_packet
(
    lwlibav_audio_decode_handler_t *adhp,
    AVPacket                       *alt_pkt,
    AVPacket                       *pkt
)
{
    /* Workaround for preventing some weird ADTS AAC decoding.
     * This should be fixed in libav/ffmpeg though. */
    AVStream *stream = adhp->format->streams[ pkt->stream_index ];
    int adts_aac = (stream->codec->codec_id == AV_CODEC_ID_AAC && stream->nb_frames == 0);
    if( !adhp->bsf && adts_aac )
        adhp->bsf = av_bitstream_filter_init( "aac_adtstoasc" );
    else if( adhp->bsf && !adts_aac )
        cleanup_av_bitstream_filter( adhp );
    if( adhp->bsf )
    {
        av_free_packet( alt_pkt );
        uint8_t *data = NULL;
        int      size = 0;
        int ret = av_bitstream_filter_filter( adhp->bsf, adhp->ctx, NULL,
                                              &data, &size,
                                              pkt->data, pkt->size, 0 );
        if( ret == 0 )
        {
            av_packet_copy_props( alt_pkt, pkt );
            alt_pkt->data = data;
            alt_pkt->size = size;
            alt_pkt->buf  = NULL;
            return;
        }
        else if( ret > 0 )
        {
             if( av_packet_copy_props( alt_pkt, pkt ) == 0
              && av_packet_from_data( alt_pkt, data, size ) == 0 )
                return;
            av_free( data );
        }
        lw_copy_av_packet( alt_pkt, pkt );
    }
    else
        *alt_pkt = *pkt;
}

static uint32_t get_audio_rap
(
    lwlibav_audio_decode_handler_t *adhp,
    uint32_t                        frame_number
)
{
    if( frame_number > adhp->frame_count )
        return 0;
    /* Get an unique value of the closest past audio keyframe. */
    uint32_t rap_number = frame_number;
    while( rap_number && !adhp->frame_list[rap_number].keyframe )
        --rap_number;
    if( rap_number == 0 )
        rap_number = 1;
    return rap_number;
}

static uint32_t shift_current_frame_number_pos
(
    audio_frame_info_t *info,
    AVPacket           *pkt,
    uint32_t            i,      /* frame_number */
    uint32_t            goal
)
{
    if( info[i].file_offset == pkt->pos )
        return i;
    if( pkt->pos > info[i].file_offset )
    {
        while( (pkt->pos != info[++i].file_offset) && i <= goal );
        if( i > goal )
            return 0;
    }
    else
    {
        while( (pkt->dts != info[--i].file_offset) && i );
        if( i == 0 )
            return 0;
    }
    return i;
}

/* Note: for PTS based seek, there is no assumption that future prediction like B-picture is present. */
#define SHIFT_CURRENT_FRAME_NUMBER_TS( TS )                         \
    static uint32_t shift_current_frame_number_##TS                 \
    (                                                               \
        audio_frame_info_t *info,                                   \
        AVPacket           *pkt,                                    \
        uint32_t            i,      /* frame_number */              \
        uint32_t            goal                                    \
    )                                                               \
    {                                                               \
        if( info[i].TS == AV_NOPTS_VALUE || info[i].TS == pkt->TS ) \
            return i;                                               \
        if( pkt->TS > info[i].TS )                                  \
        {                                                           \
            while( (pkt->TS != info[++i].TS) && i <= goal );        \
            if( i > goal )                                          \
                return 0;                                           \
        }                                                           \
        else                                                        \
        {                                                           \
            while( (pkt->TS != info[--i].TS) && i );                \
            if( i == 0 )                                            \
                return 0;                                           \
        }                                                           \
        return i;                                                   \
    }

SHIFT_CURRENT_FRAME_NUMBER_TS( pts )
SHIFT_CURRENT_FRAME_NUMBER_TS( dts )

#undef SHIFT_CURRENT_FRAME_NUMBER_TS

static void no_output_audio_decoding
(
    AVCodecContext *ctx,
    AVPacket       *pkt,
    AVFrame        *picture
)
{
    do
    {
        int dummy;
        int consumed_bytes = avcodec_decode_audio4( ctx, picture, &dummy, pkt );
        if( consumed_bytes < 0 )
            return;
        if( pkt->data )
        {
            pkt->size -= consumed_bytes;
            pkt->data += consumed_bytes;
        }
    } while( pkt->size > 0 );
}

/* This function seeks the requested frame and get it. */
static uint32_t seek_audio
(
    lwlibav_audio_decode_handler_t *adhp,
    uint32_t                        frame_number,
    uint32_t                        past_rap_number,
    AVPacket                       *pkt,
    AVFrame                        *picture
)
{
#define MAX_ERROR_COUNT 3   /* arbitrary */
    int error_count = 0;
retry_seek:;
    uint32_t rap_number = past_rap_number == 0 ? get_audio_rap( adhp, frame_number ) : past_rap_number;
    if( rap_number == 0 )
        return 0;
    int64_t rap_pos = (adhp->lw_seek_flags & SEEK_POS_BASED) ? adhp->frame_list[rap_number].file_offset
                    : (adhp->lw_seek_flags & SEEK_PTS_BASED) ? adhp->frame_list[rap_number].pts
                    : (adhp->lw_seek_flags & SEEK_DTS_BASED) ? adhp->frame_list[rap_number].dts
                    :                                          adhp->frame_list[rap_number].sample_number;
    /* Seek to audio keyframe.
     * Note: av_seek_frame() for DV in AVI Type-1 requires stream_index = 0. */
    int flags = (adhp->lw_seek_flags & SEEK_POS_BASED) ? AVSEEK_FLAG_BYTE : adhp->lw_seek_flags == 0 ? AVSEEK_FLAG_FRAME : 0;
    int stream_index = adhp->dv_in_avi == 1 ? 0 : adhp->stream_index;
    if( av_seek_frame( adhp->format, stream_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD ) < 0 )
        av_seek_frame( adhp->format, stream_index, rap_pos, flags | AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY );
    /* Seek to the target audio frame and get it. */
    int match = 0;
    for( uint32_t i = rap_number; i <= frame_number; )
    {
        if( match && picture && adhp->exh.current_index == adhp->frame_list[i - 1].extradata_index )
        {
            /* Actual decoding to establish stability of subsequent decoding. */
            AVPacket *alter_pkt = &adhp->alter_packet;
            make_decodable_packet( adhp, alter_pkt, pkt );
            no_output_audio_decoding( adhp->ctx, alter_pkt, picture );
        }
        if( lwlibav_get_av_frame( adhp->format, adhp->stream_index, i, pkt ) )
            break;
        if( !match && error_count <= MAX_ERROR_COUNT )
        {
            /* Shift the current frame number in order to match file offset, PTS or DTS
             * since libavformat might have sought wrong position. */
            if( adhp->lw_seek_flags & SEEK_POS_BASED )
            {
                if( pkt->pos == -1 || adhp->frame_list[i].file_offset == -1 )
                    continue;
                i = shift_current_frame_number_pos( adhp->frame_list, pkt, i, frame_number );
            }
            else if( adhp->lw_seek_flags & SEEK_PTS_BASED )
            {
                if( pkt->pts == AV_NOPTS_VALUE )
                    continue;
                i = shift_current_frame_number_pts( adhp->frame_list, pkt, i, frame_number );
            }
            else if( adhp->lw_seek_flags & SEEK_DTS_BASED )
            {
                if( pkt->dts == AV_NOPTS_VALUE )
                    continue;
                i = shift_current_frame_number_dts( adhp->frame_list, pkt, i, frame_number );
            }
            if( i == 0 )
            {
                /* Retry to seek from more past audio keyframe. */
                past_rap_number = get_audio_rap( adhp, rap_number - 1 );
                ++error_count;
                goto retry_seek;
            }
            match = 1;
        }
        ++i;
    }
    return rap_number;
#undef MAX_ERROR_COUNT
}

uint64_t lwlibav_get_pcm_audio_samples
(
    lwlibav_audio_decode_handler_t *adhp,
    lwlibav_audio_output_handler_t *aohp,
    void                           *buf,
    int64_t                         start,
    int64_t                         wanted_length
)
{
    if( adhp->error )
        return 0;
    uint32_t               frame_number;
    uint32_t               rap_number      = 0;
    uint32_t               past_rap_number = 0;
    uint64_t               output_length   = 0;
    enum audio_output_flag output_flags    = AUDIO_OUTPUT_NO_FLAGS;
    AVPacket              *pkt       = &adhp->packet;
    AVPacket              *alter_pkt = &adhp->alter_packet;
    int                    already_gotten;
    aohp->request_length = wanted_length;
    if( start > 0 && start == adhp->next_pcm_sample_number )
    {
        frame_number   = adhp->last_frame_number;
        output_length += output_pcm_samples_from_buffer( aohp, adhp->frame_buffer, (uint8_t **)&buf, &output_flags );
        if( output_flags & AUDIO_OUTPUT_ENOUGH )
            goto audio_out;
        if( alter_pkt->size <= 0 )
            ++frame_number;
        aohp->output_sample_offset = 0;
        already_gotten             = 0;
    }
    else
    {
        /* Seek audio stream. */
        adhp->next_pcm_sample_number = 0;
        adhp->last_frame_number      = 0;
        /* Get frame_number. */
        uint64_t start_frame_pos;
        if( start >= 0 )
            start_frame_pos = start;
        else
        {
            uint64_t silence_length = -start;
            put_silence_audio_samples( (int)(silence_length * aohp->output_block_align), aohp->output_bits_per_sample == 8, (uint8_t **)&buf );
            output_length        += silence_length;
            aohp->request_length -= silence_length;
            start_frame_pos = 0;
        }
        frame_number = find_start_audio_frame( adhp, aohp->output_sample_rate, start_frame_pos, &aohp->output_sample_offset );
retry_seek:
        av_free_packet( pkt );
        cleanup_av_bitstream_filter( adhp );
        /* Flush audio resampler buffers. */
        if( flush_resampler_buffers( aohp->avr_ctx ) < 0 )
        {
            adhp->error = 1;
            if( adhp->lh.show_log )
                adhp->lh.show_log( &adhp->lh, LW_LOG_FATAL,
                                   "Failed to flush resampler buffers.\n"
                                   "It is recommended you reopen the file." );
            return 0;
        }
        /* Flush audio decoder buffers. */
        lwlibav_extradata_handler_t *exhp = &adhp->exh;
        int extradata_index = adhp->frame_list[frame_number].extradata_index;
        if( extradata_index != exhp->current_index )
        {
            /* Update the extradata. */
            rap_number = get_audio_rap( adhp, frame_number );
            assert( rap_number != 0 );
            lwlibav_update_configuration( (lwlibav_decode_handler_t *)adhp, rap_number, extradata_index, 0 );
        }
        else
            lwlibav_flush_buffers( (lwlibav_decode_handler_t *)adhp );
        if( adhp->error )
            return 0;
        /* Seek and get a audio packet. */
        rap_number = seek_audio( adhp, frame_number, past_rap_number, pkt, output_flags != AUDIO_OUTPUT_NO_FLAGS ? adhp->frame_buffer : NULL );
        already_gotten = 1;
    }
    do
    {
        if( already_gotten )
        {
            already_gotten = 0;
            make_decodable_packet( adhp, alter_pkt, pkt );
        }
        else if( frame_number > adhp->frame_count )
        {
            av_free_packet( pkt );
            if( adhp->exh.delay_count )
            {
                /* Null packet */
                av_init_packet( pkt );
                make_null_packet( pkt );
                *alter_pkt = *pkt;
                -- adhp->exh.delay_count;
            }
            else
                goto audio_out;
        }
        else if( alter_pkt->size <= 0 )
        {
            /* Getting an audio packet must be after flushing all remaining samples in resampler's FIFO buffer. */
            lwlibav_get_av_frame( adhp->format, adhp->stream_index, frame_number, pkt );
            make_decodable_packet( adhp, alter_pkt, pkt );
        }
        /* Decode and output from an audio packet. */
        output_flags   = AUDIO_OUTPUT_NO_FLAGS;
        output_length += output_pcm_samples_from_packet( aohp, adhp->ctx, alter_pkt, adhp->frame_buffer, (uint8_t **)&buf, &output_flags );
        if( output_flags & AUDIO_DECODER_DELAY )
        {
            if( rap_number > 1 && (output_flags & AUDIO_DECODER_ERROR) )
            {
                /* Retry to seek from more past audio keyframe because libavformat might have failed seek.
                 * This operation occurs only at the first decoding time after seek. */
                past_rap_number = get_audio_rap( adhp, rap_number - 1 );
                if( past_rap_number
                 && past_rap_number < rap_number )
                    goto retry_seek;
            }
            ++ adhp->exh.delay_count;
        }
        else
            /* Disable seek retry. */
            rap_number = 0;
        if( output_flags & AUDIO_RECONFIG_FAILURE )
        {
            adhp->error = 1;
            if( adhp->lh.show_log )
                adhp->lh.show_log( &adhp->lh, LW_LOG_FATAL,
                                   "Failed to reconfigure resampler.\n"
                                   "It is recommended you reopen the file." );
            goto audio_out;
        }
        if( output_flags & AUDIO_OUTPUT_ENOUGH )
            goto audio_out;
        ++frame_number;
    } while( 1 );
audio_out:
    adhp->next_pcm_sample_number = start + output_length;
    adhp->last_frame_number      = frame_number;
    return output_length;
}

void lwlibav_cleanup_audio_decode_handler( lwlibav_audio_decode_handler_t *adhp )
{
    lwlibav_extradata_handler_t *exhp = &adhp->exh;
    if( exhp->entries )
    {
        for( int i = 0; i < exhp->entry_count; i++ )
            if( exhp->entries[i].extradata )
                av_free( exhp->entries[i].extradata );
        free( exhp->entries );
    }
    cleanup_av_bitstream_filter( adhp );
    av_free_packet( &adhp->packet );
    if( adhp->frame_list )
        lw_freep( &adhp->frame_list );
    if( adhp->index_entries )
        av_freep( &adhp->index_entries );
    if( adhp->frame_buffer )
        av_frame_free( &adhp->frame_buffer );
    if( adhp->ctx )
    {
        avcodec_close( adhp->ctx );
        adhp->ctx = NULL;
    }
    if( adhp->format )
        lavf_close_file( &adhp->format );
}

void set_audio_basic_settings
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number
)
{
    lwlibav_audio_decode_handler_t *adhp = (lwlibav_audio_decode_handler_t *)dhp;
    AVCodecContext *ctx = adhp->format->streams[ adhp->stream_index ]->codec;
    lwlibav_extradata_t *entry = &adhp->exh.entries[ adhp->frame_list[frame_number].extradata_index ];
    ctx->sample_rate           = entry->sample_rate;
    ctx->channel_layout        = entry->channel_layout;
    ctx->sample_fmt            = entry->sample_format;
    ctx->bits_per_coded_sample = entry->bits_per_sample;
    ctx->block_align           = entry->block_align;
    ctx->channels              = av_get_channel_layout_nb_channels( ctx->channel_layout );
}

int try_decode_audio_frame
(
    lwlibav_decode_handler_t *dhp,
    uint32_t                  frame_number,
    char                     *error_string
)
{
    AVFrame *picture = av_frame_alloc();
    if( !picture )
    {
        strcpy( error_string, "Failed to alloc AVFrame to set up a decoder configuration.\n" );
        return -1;
    }
    lwlibav_audio_decode_handler_t *adhp = (lwlibav_audio_decode_handler_t *)dhp;
    AVFormatContext *format_ctx   = adhp->format;
    int              stream_index = adhp->stream_index;
    AVPacket        *pkt          = &adhp->packet;
    AVPacket        *alter_pkt    = &adhp->alter_packet;
    AVCodecContext  *ctx          = format_ctx->streams[stream_index]->codec;
    uint32_t         start_frame  = frame_number;
    int              err          = 0;
    do
    {
        if( frame_number > adhp->frame_count )
            break;
        /* Get a frame. */
        int extradata_index = adhp->frame_list[frame_number].extradata_index;
        if( extradata_index != adhp->exh.current_index )
            break;
        if( frame_number == start_frame )
            seek_audio( adhp, frame_number, 0, pkt, NULL );
        else
        {
            int ret = lwlibav_get_av_frame( format_ctx, stream_index, frame_number, pkt );
            if( ret > 0 )
                break;
            else if( ret < 0 )
            {
                if( ctx->sample_rate == 0 )
                    strcpy( error_string, "Failed to set up sample rate.\n" );
                else if( ctx->channel_layout == 0 && ctx->channels == 0 )
                    strcpy( error_string, "Failed to set up channels.\n" );
                else
                    strcpy( error_string, "Failed to set up sample format.\n" );
                err = -1;
                goto abort;
            }
        }
        make_decodable_packet( adhp, alter_pkt, pkt );
        /* Try decode a frame. */
        no_output_audio_decoding( ctx, alter_pkt, picture );
        ++frame_number;
    } while( ctx->sample_rate == 0
          || ctx->sample_fmt == AV_SAMPLE_FMT_NONE
          || (ctx->channels == 0 && ctx->channel_layout == 0)
          || (ctx->channels != av_get_channel_layout_nb_channels( ctx->channel_layout )) );
abort:
    cleanup_av_bitstream_filter( adhp );
    av_frame_free( &picture );
    return err;
}
