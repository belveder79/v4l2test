// ����ͷ�ļ�
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include "ffencoder.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/avutil.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

//++ frame dropper
typedef struct {
    int x;
    int y;
    int dx;
    int dy;
    int e;
} FRAMEDROPPER;

void frame_dropper_init   (FRAMEDROPPER *pfd, int frate_in, int frate_out);
int  frame_dropper_clocked(FRAMEDROPPER *pfd);

void frame_dropper_init(FRAMEDROPPER *pfd, int frate_in, int frate_out)
{
    pfd->x  = 0;
    pfd->y  = 0;
    pfd->e  = 0;
    pfd->dx = frate_in;
    pfd->dy = frate_out;
}

int frame_dropper_clocked(FRAMEDROPPER *pfd)
{
    pfd->x++;
    pfd->e += pfd->dy;
    if (pfd->e * 2 >= pfd->dx) {
        pfd->y++;
        pfd->e -= pfd->dx;
//      printf("%d, %d\n", pfd->x, pfd->y);
        return 1;
    }
    return 0;
}
//-- frame dropper

// �ڲ����Ͷ���
typedef struct
{
    FFENCODER_PARAMS   params;

    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;

    AVStream          *astream;
    AVFrame           *aframes;
    int64_t            next_apts;
    AVFrame           *aframecur;
    uint8_t           *adatacur[8];
    int                asamplenum;
    sem_t              asemr;
    sem_t              asemw;
    int                ahead;
    int                atail;

    AVStream          *vstream;
    AVFrame           *vframes;
    int64_t            next_vpts;
    sem_t              vsemr;
    sem_t              vsemw;
    int                vhead;
    int                vtail;
    FRAMEDROPPER       vdropper;

    AVFormatContext   *ofctxt;
    AVCodec           *acodec;
    AVCodec           *vcodec;
    AVDictionary      *avopt;

    int                have_audio;
    int                have_video;

    #define FFENCODER_TS_EXIT    (1 << 0)
    int                thread_state;
    pthread_t          aencode_thread_id;
    pthread_t          vencode_thread_id;
    pthread_mutex_t    mutex;
} FFENCODER;

// �ڲ�ȫ�ֱ�������
static FFENCODER_PARAMS DEF_FFENCODER_PARAMS =
{
    // input params
    AV_CH_LAYOUT_STEREO,        // in_audio_channel_layout
    AV_SAMPLE_FMT_S16,          // in_audio_sample_fmt
    44100,                      // in_audio_sample_rate
    640,                        // in_video_width
    480,                        // in_video_height
    AV_PIX_FMT_YUYV422,         // in_video_pixfmt
    30,                         // in_video_frame_rate

    // output params
    (char*)"/sdcard/test.mp4",  // filename
    32000,                      // out_audio_bitrate
    AV_CH_LAYOUT_MONO,          // out_audio_channel_layout
    44100,                      // out_audio_sample_rate
    256000,                     // out_video_bitrate
    320,                        // out_video_width
    240,                        // out_video_height
    20,                         // out_video_frame_rate

    // other params
    0,                          // start_apts
    0,                          // start_vpts
    SWS_FAST_BILINEAR,          // scale_flags
    5,                          // audio_buffer_number
    5,                          // video_buffer_number
};

// �ڲ�����ʵ��
static int write_frame(FFENCODER *encoder, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
    int ret;

    /* rescale output packet timestamp values from codec to stream timebase */
    av_packet_rescale_ts(pkt, *time_base, st->time_base);
    pkt->stream_index = st->index;

    pthread_mutex_lock  (&encoder->mutex);
    ret = av_interleaved_write_frame(encoder->ofctxt, pkt);
    pthread_mutex_unlock(&encoder->mutex);

    return ret;
}

static void* audio_encode_thread_proc(void *param)
{
    FFENCODER *encoder = (FFENCODER*)param;
    AVFrame   *aframe  = NULL;
    AVPacket   pkt;
    int        got     =  0;
    int        ret     =  0;

    memset(&pkt, 0, sizeof(AVPacket));

    while (1) {
        sem_wait(&encoder->asemr);
        aframe = &encoder->aframes[encoder->ahead];

        if (encoder->thread_state & FFENCODER_TS_EXIT) {
            break;
        }

        // encode audio
        ret = avcodec_encode_audio2(encoder->astream->codec, &pkt, aframe, &got);
        if (ret < 0) {
            printf("error encoding audio frame !\n");
            exit(1);
        }

        // write audio
        if (got) {
            write_frame(encoder, &encoder->astream->codec->time_base, encoder->astream, &pkt);
        }

        if (++encoder->ahead == encoder->params.audio_buffer_number) {
            encoder->ahead = 0;
        }
        sem_post(&encoder->asemw);
    }

    do {
        avcodec_encode_audio2(encoder->astream->codec, &pkt, NULL, &got);
        if (got) {
            write_frame(encoder, &encoder->astream->codec->time_base, encoder->astream, &pkt);
        }
    } while (got);

    return NULL;
}

static void* video_encode_thread_proc(void *param)
{
    FFENCODER *encoder = (FFENCODER*)param;
    AVFrame   *vframe  = NULL;
    AVPacket   pkt;
    int        got     =  0;
    int        ret     =  0;

    memset(&pkt, 0, sizeof(AVPacket));

    while (1) {
        sem_wait(&encoder->vsemr);
        vframe = &encoder->vframes[encoder->vhead];

        if (encoder->thread_state & FFENCODER_TS_EXIT) {
            break;
        }

        // encode & write video
        if (encoder->ofctxt->oformat->flags & AVFMT_RAWPICTURE) {
            /* a hack to avoid data copy with some raw video muxers */
            pkt.flags |= AV_PKT_FLAG_KEY;
            pkt.data   = (uint8_t*)vframe;
            pkt.size   = sizeof(AVPicture);
            pkt.pts    = vframe->pts;
            pkt.dts    = vframe->pts;
            write_frame(encoder, &encoder->vstream->codec->time_base, encoder->vstream, &pkt);
        } else {
            /* encode the image */
            ret = avcodec_encode_video2(encoder->vstream->codec, &pkt, vframe, &got);
            if (ret < 0) {
                printf("error encoding video frame !\n");
                exit(1);
            }

            if (got) {
                write_frame(encoder, &encoder->vstream->codec->time_base, encoder->vstream, &pkt);
            }
        }

        if (ret < 0) {
            printf("error while writing video frame !\n");
            exit(1);
        }

        if (++encoder->vhead == encoder->params.video_buffer_number) {
            encoder->vhead = 0;
        }
        sem_post(&encoder->vsemw);
    }

    do {
        avcodec_encode_video2(encoder->vstream->codec, &pkt, NULL, &got);
        if (got) {
            write_frame(encoder, &encoder->vstream->codec->time_base, encoder->vstream, &pkt);
        }
    } while (got);

    return NULL;
}

static int add_astream(FFENCODER *encoder)
{
    enum AVCodecID  codec_id = encoder->ofctxt->oformat->audio_codec;
    AVCodecContext *c        = NULL;
    int i;

    if (codec_id == AV_CODEC_ID_NONE) return 0;

    encoder->acodec = avcodec_find_encoder(codec_id);
    if (!encoder->acodec) {
        printf("could not find encoder for '%s'\n", avcodec_get_name(codec_id));
        return -1;
    }

    encoder->astream = avformat_new_stream(encoder->ofctxt, encoder->acodec);
    if (!encoder->astream) {
        printf("could not allocate stream\n");
        return -1;
    }

    encoder->astream->id = encoder->ofctxt->nb_streams - 1;
    c                    = encoder->astream->codec;

    c->sample_fmt  = encoder->acodec->sample_fmts ? encoder->acodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    c->bit_rate    = encoder->params.out_audio_bitrate;
    c->sample_rate = encoder->params.out_audio_sample_rate;
    if (encoder->acodec->supported_samplerates)
    {
        c->sample_rate = encoder->acodec->supported_samplerates[0];
        for (i=0; encoder->acodec->supported_samplerates[i]; i++) {
            if (encoder->acodec->supported_samplerates[i] == encoder->params.out_audio_sample_rate)
                c->sample_rate = encoder->params.out_audio_sample_rate;
        }
    }

    c->channel_layout = encoder->params.out_audio_channel_layout;
    if (encoder->acodec->channel_layouts)
    {
        c->channel_layout = encoder->acodec->channel_layouts[0];
        for (i=0; encoder->acodec->channel_layouts[i]; i++) {
            if ((int)encoder->acodec->channel_layouts[i] == encoder->params.out_audio_channel_layout)
                c->channel_layout = encoder->params.out_audio_channel_layout;
        }
    }
    c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
    encoder->astream->time_base.num = 1;
    encoder->astream->time_base.den = c->sample_rate;

    /* some formats want stream headers to be separate. */
    if (encoder->ofctxt->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    encoder->have_audio = 1;
    return 0;
}

static int add_vstream(FFENCODER *encoder)
{
    enum AVCodecID  codec_id = encoder->ofctxt->oformat->video_codec;
    AVCodecContext *c        = NULL;

    if (codec_id == AV_CODEC_ID_NONE) return 0;

    encoder->vcodec = avcodec_find_encoder(codec_id);
    if (!encoder->vcodec) {
        printf("could not find encoder for '%s'\n", avcodec_get_name(codec_id));
        return -1;
    }

    encoder->vstream = avformat_new_stream(encoder->ofctxt, encoder->vcodec);
    if (!encoder->vstream) {
        printf("could not allocate stream\n");
        return -1;
    }

    encoder->vstream->id = encoder->ofctxt->nb_streams - 1;
    c                    = encoder->vstream->codec;

    c->codec_id = codec_id;
    c->bit_rate = encoder->params.out_video_bitrate;
    /* Resolution must be a multiple of two. */
    c->width    = encoder->params.out_video_width;
    c->height   = encoder->params.out_video_height;
    /* timebase: This is the fundamental unit of time (in seconds) in terms
     * of which frame timestamps are represented. For fixed-fps content,
     * timebase should be 1/framerate and timestamp increments should be
     * identical to 1. */
    encoder->vstream->time_base.num = 1;
    encoder->vstream->time_base.den = encoder->params.out_video_frame_rate;
    c->time_base = encoder->vstream->time_base;
    c->gop_size  = 12; /* emit one intra frame every twelve frames at most */
    c->pix_fmt   = AV_PIX_FMT_YUV420P;
    if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
        /* just for testing, we also add B frames */
        c->max_b_frames = 2;
    }
    if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        /* needed to avoid using macroblocks in which some coeffs overflow.
         * This does not happen with normal video, it just happens here as
         * the motion of the chroma plane does not match the luma plane. */
        c->mb_decision = 2;
    }

    /* some formats want stream headers to be separate. */
    if (encoder->ofctxt->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= CODEC_FLAG_GLOBAL_HEADER;

    encoder->have_video = 1;
    return 0;
}

static void alloc_audio_frame(AVFrame *frame, enum AVSampleFormat sample_fmt, uint64_t channel_layout, int sample_rate, int nb_samples)
{
    int ret;

    frame->format         = sample_fmt;
    frame->channel_layout = channel_layout;
    frame->sample_rate    = sample_rate;
    frame->nb_samples     = nb_samples;

    if (nb_samples) {
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0) {
            printf("error allocating an audio buffer\n");
            exit(1);
        }
    }
}

static void open_audio(FFENCODER *encoder)
{
    AVCodec        *codec     = encoder->acodec;
    AVDictionary   *opt_arg   = encoder->avopt;
    AVCodecContext *c         = encoder->astream->codec;
    AVDictionary   *opt       = NULL;
    int             in_layout = encoder->params.in_audio_channel_layout;
    AVSampleFormat  in_sfmt   = (AVSampleFormat)encoder->params.in_audio_sample_fmt;
    int             in_rate   = encoder->params.in_audio_sample_rate;
    int             in_chnb   = av_get_channel_layout_nb_channels(in_layout);
    int             i, ret;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        printf("could not open audio codec !\n");
        exit(1);
    }

    /* create resampler context */
    encoder->swr_ctx = swr_alloc();
    if (!encoder->swr_ctx) {
        printf("could not allocate resampler context\n");
        exit(1);
    }

    /* set options */
    av_opt_set_int       (encoder->swr_ctx, "in_channel_count",  in_chnb,        0);
    av_opt_set_int       (encoder->swr_ctx, "in_sample_rate",    in_rate,        0);
    av_opt_set_sample_fmt(encoder->swr_ctx, "in_sample_fmt",     in_sfmt,        0);
    av_opt_set_int       (encoder->swr_ctx, "out_channel_count", c->channels,    0);
    av_opt_set_int       (encoder->swr_ctx, "out_sample_rate",   c->sample_rate, 0);
    av_opt_set_sample_fmt(encoder->swr_ctx, "out_sample_fmt",    c->sample_fmt,  0);

    /* initialize the resampling context */
    if ((ret = swr_init(encoder->swr_ctx)) < 0) {
        printf("failed to initialize the resampling context\n");
        exit(1);
    }

    encoder->aframes = (AVFrame*)malloc(sizeof(AVFrame) * encoder->params.audio_buffer_number);
    if (!encoder->aframes) {
        printf("failed to allocate memory for aframes !\n");
        exit(1);
    }
    else {
        memset(encoder->aframes, 0, sizeof(AVFrame) * encoder->params.audio_buffer_number);
    }
    for (i=0; i<encoder->params.audio_buffer_number; i++) {
        alloc_audio_frame(&encoder->aframes[i],
            c->sample_fmt,
            c->channel_layout,
            c->sample_rate,
            c->frame_size);
    }

    sem_init(&encoder->asemr, 0, 0                                  );
    sem_init(&encoder->asemw, 0, encoder->params.audio_buffer_number);

    // create audio encoding thread
    pthread_create(&encoder->aencode_thread_id, NULL, audio_encode_thread_proc, encoder);
}

static void alloc_picture(AVFrame *picture, enum AVPixelFormat pix_fmt, int width, int height)
{
    int ret;

    picture->format = pix_fmt;
    picture->width  = width;
    picture->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(picture, 32);
    if (ret < 0) {
        printf("could not allocate frame data.\n");
        exit(1);
    }
}

static void open_video(FFENCODER *encoder)
{
    AVCodec        *codec   = encoder->vcodec;
    AVDictionary   *opt_arg = encoder->avopt;
    AVCodecContext *c       = encoder->vstream->codec;
    AVDictionary   *opt     = NULL;
    int             i, ret;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        printf("could not open video codec !\n");
        exit(1);
    }

    encoder->sws_ctx = sws_getContext(
        encoder->params.in_video_width,
        encoder->params.in_video_height,
        (AVPixelFormat)encoder->params.in_video_pixfmt,
        c->width,
        c->height,
        (AVPixelFormat)c->pix_fmt,
        encoder->params.scale_flags,
        NULL, NULL, NULL);
    if (!encoder->sws_ctx) {
        printf("could not initialize the conversion context\n");
        exit(1);
    }

    encoder->vframes = (AVFrame*)malloc(sizeof(AVFrame) * encoder->params.video_buffer_number);
    if (!encoder->vframes) {
        printf("failed to allocate memory for vframes !\n");
        exit(1);
    }
    else {
        memset(encoder->vframes, 0, sizeof(AVFrame) * encoder->params.video_buffer_number);
    }
    for (i=0; i<encoder->params.video_buffer_number; i++) {
        alloc_picture(&encoder->vframes[i], c->pix_fmt, c->width, c->height);
    }

    sem_init(&encoder->vsemr, 0, 0                                  );
    sem_init(&encoder->vsemw, 0, encoder->params.video_buffer_number);

    // create video encoding thread
    pthread_create(&encoder->vencode_thread_id, NULL, video_encode_thread_proc, encoder);
}

static void close_astream(FFENCODER *encoder)
{
    int i;

    encoder->thread_state |= FFENCODER_TS_EXIT;
    sem_post(&encoder->asemr);
    pthread_join(encoder->aencode_thread_id, NULL);

    sem_destroy(&encoder->asemr);
    sem_destroy(&encoder->asemw);

    //++ for audio frames
    for (i=0; i<encoder->params.audio_buffer_number; i++) {
        av_frame_unref(&encoder->aframes[i]);
    }
    free(encoder->aframes);
    //-- for audio frames

    avcodec_close(encoder->astream->codec);
    swr_free(&encoder->swr_ctx);
}

static void close_vstream(FFENCODER *encoder)
{
    int i;

    encoder->thread_state |= FFENCODER_TS_EXIT;
    sem_post(&encoder->vsemr);
    pthread_join(encoder->vencode_thread_id, NULL);

    sem_destroy(&encoder->vsemr);
    sem_destroy(&encoder->vsemw);

    //++ for video frames
    for (i=0; i<encoder->params.video_buffer_number; i++) {
        av_frame_unref(&encoder->vframes[i]);
    }
    free(encoder->vframes);
    //-- for video frames

    avcodec_close(encoder->vstream->codec);
    sws_freeContext(encoder->sws_ctx);
}


// ����ʵ��
void* ffencoder_init(FFENCODER_PARAMS *params)
{
    int ret;

    // allocate context for ffencoder
    FFENCODER *encoder = (FFENCODER*)malloc(sizeof(FFENCODER));
    if (encoder) memset(encoder, 0, sizeof(FFENCODER));
    else return NULL;

    // using default params if not set
    if (!params                          ) params                          = &DEF_FFENCODER_PARAMS;
    if (!params->in_audio_channel_layout ) params->in_audio_channel_layout = DEF_FFENCODER_PARAMS.in_audio_channel_layout;
    if (!params->in_audio_sample_fmt     ) params->in_audio_sample_fmt     = DEF_FFENCODER_PARAMS.in_audio_sample_fmt;
    if (!params->in_audio_sample_rate    ) params->in_audio_sample_rate    = DEF_FFENCODER_PARAMS.in_audio_sample_rate;
    if (!params->in_video_width          ) params->in_video_width          = DEF_FFENCODER_PARAMS.in_video_width;
    if (!params->in_video_height         ) params->in_video_height         = DEF_FFENCODER_PARAMS.in_video_height;
    if (!params->in_video_pixfmt         ) params->in_video_pixfmt         = DEF_FFENCODER_PARAMS.in_video_pixfmt;
    if (!params->in_video_frame_rate     ) params->in_video_frame_rate     = DEF_FFENCODER_PARAMS.in_video_frame_rate;
    if (!params->out_filename            ) params->out_filename            = DEF_FFENCODER_PARAMS.out_filename;
    if (!params->out_audio_bitrate       ) params->out_audio_bitrate       = DEF_FFENCODER_PARAMS.out_audio_bitrate;
    if (!params->out_audio_channel_layout) params->out_audio_channel_layout= DEF_FFENCODER_PARAMS.out_audio_channel_layout;
    if (!params->out_audio_sample_rate   ) params->out_audio_sample_rate   = DEF_FFENCODER_PARAMS.out_audio_sample_rate;
    if (!params->out_video_bitrate       ) params->out_video_bitrate       = DEF_FFENCODER_PARAMS.out_video_bitrate;
    if (!params->out_video_width         ) params->out_video_width         = DEF_FFENCODER_PARAMS.out_video_width;
    if (!params->out_video_height        ) params->out_video_height        = DEF_FFENCODER_PARAMS.out_video_height;
    if (!params->out_video_frame_rate    ) params->out_video_frame_rate    = DEF_FFENCODER_PARAMS.out_video_frame_rate;
    if (!params->start_apts              ) params->start_apts              = DEF_FFENCODER_PARAMS.start_apts;
    if (!params->start_vpts              ) params->start_vpts              = DEF_FFENCODER_PARAMS.start_vpts;
    if (!params->scale_flags             ) params->scale_flags             = DEF_FFENCODER_PARAMS.scale_flags;
    if (!params->audio_buffer_number     ) params->audio_buffer_number     = DEF_FFENCODER_PARAMS.audio_buffer_number;
    if (!params->video_buffer_number     ) params->video_buffer_number     = DEF_FFENCODER_PARAMS.video_buffer_number;
    memcpy(&encoder->params, params, sizeof(FFENCODER_PARAMS));
    encoder->next_apts = params->start_apts;
    encoder->next_vpts = params->start_vpts;

    /* initialize libavcodec, and register all codecs and formats. */
    av_register_all();

    /* allocate the output media context */
    avformat_alloc_output_context2(&encoder->ofctxt, NULL, NULL, params->out_filename);
    if (!encoder->ofctxt)
    {
        printf("could not deduce output format from file extension: using MPEG.\n");
        goto failed;
    }

    /* add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (add_astream(encoder) < 0)
    {
        printf("failed to add audio stream.\n");
        goto failed;
    }

    if (add_vstream(encoder) < 0)
    {
        printf("failed to add video stream.\n");
        goto failed;
    }

    // init mutex for audio/video encoding thread
    pthread_mutex_init(&encoder->mutex, NULL);

    /* now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (encoder->have_audio) open_audio(encoder);
    if (encoder->have_video) open_video(encoder);

    /* open the output file, if needed */
    if (!(encoder->ofctxt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&encoder->ofctxt->pb, params->out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            printf("could not open '%s' !\n", params->out_filename);
            goto failed;
        }
    }

    /* write the stream header, if any. */
    ret = avformat_write_header(encoder->ofctxt, &encoder->avopt);
    if (ret < 0) {
        printf("error occurred when opening output file !\n");
        goto failed;
    }

    // init frame dropper
    frame_dropper_init(&encoder->vdropper, params->in_video_frame_rate, params->out_video_frame_rate);

    // successed
    return encoder;

failed:
    ffencoder_free(encoder);
    return NULL;
}

void ffencoder_free(void *ctxt)
{
    FFENCODER *encoder = (FFENCODER*)ctxt;
    if (!ctxt) return;

    /* close each codec. */
    if (encoder->have_audio) close_astream(encoder);
    if (encoder->have_video) close_vstream(encoder);

    // destroy mutex
    pthread_mutex_destroy(&encoder->mutex);

    /* write the trailer, if any. The trailer must be written before you
     * close the CodecContexts open when you wrote the header; otherwise
     * av_write_trailer() may try to use memory that was freed on
     * av_codec_close(). */
    av_write_trailer(encoder->ofctxt);

    /* close the output file. */
    if (!(encoder->ofctxt->oformat->flags & AVFMT_NOFILE)) avio_close(encoder->ofctxt->pb);

    /* free the stream */
    avformat_free_context(encoder->ofctxt);

    // free encoder context
    free(encoder);
}

int ffencoder_audio(void *ctxt, void *data[8], int nbsample)
{
    FFENCODER *encoder = (FFENCODER*)ctxt;
    AVFrame   *aframe  = NULL;
    int        sampnum, i;
    if (!ctxt) return -1;

    do {
        // resample audio
        if (encoder->asamplenum == 0) {
            sem_wait(&encoder->asemw);
            aframe = encoder->aframecur = &encoder->aframes[encoder->atail];
            for (i=0; i<8; i++) {
                encoder->adatacur[i] = aframe->data[i];
            }
            encoder->asamplenum = aframe->nb_samples;
        }
        else {
            aframe = encoder->aframecur;
        }

        sampnum  = swr_convert(encoder->swr_ctx,
                        encoder->adatacur, encoder->asamplenum,
                        (const uint8_t**)data, nbsample);
        data     = NULL;
        nbsample = 0;
        for (i=0; i<8; i++) {
            encoder->adatacur[i] += sampnum * 2 * encoder->astream->codec->channels;
        }
        encoder->asamplenum -= sampnum;

        if (encoder->asamplenum == 0) {
            AVRational r = { 1, encoder->astream->codec->sample_rate };
            encoder->aframecur->pts = encoder->next_apts;
            encoder->next_apts     += aframe->nb_samples;

            if (++encoder->atail == encoder->params.audio_buffer_number) {
                encoder->atail = 0;
            }
            sem_post(&encoder->asemr);
        }
    } while (sampnum > 0);

    return 0;
}

int ffencoder_video(void *ctxt, void *data[8], int linesize[8])
{
    FFENCODER *encoder = (FFENCODER*)ctxt;
    AVFrame   *vframe  = NULL;
    int        drop    = 0;
    if (!ctxt) return -1;

    int64_t apts = av_rescale_q(encoder->next_apts, encoder->astream->codec->time_base, (AVRational){1,1000});
    int64_t vpts = av_rescale_q(encoder->next_vpts, encoder->vstream->codec->time_base, (AVRational){1,1000});

    drop = !frame_dropper_clocked(&encoder->vdropper);
    if (vpts - apts > 100) drop = 1;
    if (apts - vpts > 100) drop = 0;
    if (drop) {
//      printf("frame dropped by frame dropper !\n");
        return 0;
    }

    sem_wait(&encoder->vsemw);
    vframe = &encoder->vframes[encoder->vtail];

    // scale video image
    sws_scale(
        encoder->sws_ctx,
        (const uint8_t * const *)data,
        linesize,
        0,
        encoder->params.in_video_height,
        vframe->data,
        vframe->linesize);
    vframe->pts = encoder->next_vpts++;

    if (++encoder->vtail == encoder->params.video_buffer_number) {
        encoder->vtail = 0;
    }

    sem_post(&encoder->vsemr);
    return 0;
}