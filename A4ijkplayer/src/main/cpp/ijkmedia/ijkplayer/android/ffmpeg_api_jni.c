/*
 * ffmpeg_api_jni.c
 *
 * Copyright (c) 2014 Bilibili
 * Copyright (c) 2014 Zhang Rui <bbcallen@gmail.com>
 *
 * This file is part of ijkPlayer.
 *
 * ijkPlayer is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * ijkPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with ijkPlayer; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "ffmpeg_api_jni.h"
#include <assert.h>
#include <string.h>
#include <jni.h>
#include "../ff_ffinc.h"
#include "ijksdl/ijksdl_log.h"
#include "ijksdl/android/ijksdl_android_jni.h"

#include "libavformat/avformat.h"

#define JNI_CLASS_FFMPEG_API "tv/danmaku/ijk/media/player/ffmpeg/FFmpegApi"

typedef struct ffmpeg_api_fields_t {
    jclass clazz;
} ffmpeg_api_fields_t;
static ffmpeg_api_fields_t g_clazz;

static jstring
FFmpegApi_av_base64_encode(JNIEnv *env, jclass clazz, jbyteArray in)
{
    jstring ret_string = NULL;
    char*   out_buffer = 0;
    int     out_size   = 0;
    jbyte*  in_buffer  = 0;
    jsize   in_size    = (*env)->GetArrayLength(env, in);
    if (in_size <= 0)
        goto fail;

    in_buffer = (*env)->GetByteArrayElements(env, in, NULL);
    if (!in_buffer)
        goto fail;

    out_size = AV_BASE64_SIZE(in_size);
    out_buffer = malloc(out_size + 1);
    if (!out_buffer)
        goto fail;
    out_buffer[out_size] = 0;

    if (!av_base64_encode(out_buffer, out_size, (const uint8_t *)in_buffer, in_size))
        goto fail;

    ret_string = (*env)->NewStringUTF(env, out_buffer);
    fail:
    if (in_buffer) {
        (*env)->ReleaseByteArrayElements(env, in, in_buffer, JNI_ABORT);
        in_buffer = NULL;
    }
    if (out_buffer) {
        free(out_buffer);
        out_buffer = NULL;
    }
    return ret_string;
}

static jboolean
FFmpegApi_h264ToMP4Converter(JNIEnv* env, jclass clazz, jstring inputPath, jstring outputPath) {
    const char* input = (*env)->GetStringUTFChars(env, inputPath, NULL);
    const char* output = (*env)->GetStringUTFChars(env, outputPath, NULL);

    AVFormatContext *inputFormatContext = NULL;
    AVFormatContext *outputFormatContext = NULL;
    AVPacket *packet = NULL;
    AVInputFormat *input_format = NULL;
    int ret = JNI_FALSE;
    int err = 0;
    char errbuf[128];
    AVDictionary *options = NULL;

    av_log_set_level(AV_LOG_DEBUG); // 设置FFmpeg日志级别为DEBUG

    av_register_all();

    ALOGV("FFmpegApi_h264ToMP4Converter input path: %s", input);
    ALOGV("FFmpegApi_h264ToMP4Converter output path: %s", output);
    ALOGV("FFmpegApi_h264ToMP4Converter av_version_info: %s", av_version_info());

    err = avformat_open_input(&inputFormatContext, input, NULL, NULL);
    if (err < 0) {
        av_strerror(err, errbuf, sizeof(errbuf));
        ALOGV("FFmpegApi_h264ToMP4Converter avformat_open_input fail %s: %s", input, errbuf);
        goto end;
    }
    if (avformat_find_stream_info(inputFormatContext, NULL) < 0) {
        ALOGV("FFmpegApi_h264ToMP4Converter avformat_find_stream_info fail");
        goto end;
    }

    // 打印输入文件的信息
    av_dump_format(inputFormatContext, 0, input, 0);

    // Open output file
    if (avformat_alloc_output_context2(&outputFormatContext, NULL, "mp4", output) < 0) {
        ALOGV("FFmpegApi_h264ToMP4Converter avformat_alloc_output_context2 fail %s", output);
        goto end;
    }

    AVStream *in_stream = inputFormatContext->streams[0];
    AVStream *out_stream = avformat_new_stream(outputFormatContext, NULL);
    if (!out_stream) {
        ALOGV("FFmpegApi_h264ToMP4Converter out_stream fail");
        goto end;
    }

    if (avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar) < 0) {
        ALOGV("FFmpegApi_h264ToMP4Converter avcodec_parameters_copy fail");
        goto end;
    }
    out_stream->codecpar->codec_tag = 0;

    // 设置时间基
    out_stream->time_base = in_stream->time_base;

    if (!(outputFormatContext->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outputFormatContext->pb, output, AVIO_FLAG_WRITE) < 0) {
            ALOGV("FFmpegApi_h264ToMP4Converter avio_open fail");
            goto end;
        }
    }

    // Write output file header
    if (avformat_write_header(outputFormatContext, NULL) < 0) {
        ALOGV("FFmpegApi_h264ToMP4Converter avformat_write_header fail");
        goto end;
    }

    packet = av_packet_alloc();
    if (!packet) {
        ALOGV("FFmpegApi_h264ToMP4Converter packet fail");
        goto end;
    }

    // Read frames from the input file and write them to the output file
    ALOGV("FFmpegApi_h264ToMP4Converter Read frames from the input file");
    while (av_read_frame(inputFormatContext, packet) >= 0) {
        if (packet->stream_index == 0) { // Only process video data
            av_packet_rescale_ts(packet, in_stream->time_base, out_stream->time_base);
            packet->stream_index = 0;
            ALOGV("FFmpegApi_h264ToMP4Converter av_read_frame process");
            if (av_interleaved_write_frame(outputFormatContext, packet) < 0) {
                ALOGV("FFmpegApi_h264ToMP4Converter av_interleaved_write_frame fail");
                goto end;
            }
        }
        av_packet_unref(packet);
    }

    // Write output file trailer
    if (av_write_trailer(outputFormatContext) < 0) {
        ALOGV("FFmpegApi_h264ToMP4Converter av_write_trailer fail");
        goto end;
    }

    ret = JNI_TRUE;

    end:
    if (packet) {
        av_packet_free(&packet);
    }
    if (inputFormatContext) {
        avformat_close_input(&inputFormatContext);
    }
    if (outputFormatContext && !(outputFormatContext->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&outputFormatContext->pb);
    }
    if (outputFormatContext) {
        avformat_free_context(outputFormatContext);
    }

    (*env)->ReleaseStringUTFChars(env, inputPath, input);
    (*env)->ReleaseStringUTFChars(env, outputPath, output);

    return ret;
}

static jboolean
FFmpegApi_convertH264ToMp4File(JNIEnv* env, jclass clazz, jstring inputPath, jstring outputPath) {

    AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket pkt;
    int ret, i;
    int stream_index = 0;
    int *stream_mapping = NULL;
    int stream_mapping_size = 0;

    int stream_index_audio=-1;
    int stream_index_video=-1;

    const char* h264File = (*env)->GetStringUTFChars(env, inputPath, NULL);
    const char* mp4File = (*env)->GetStringUTFChars(env, outputPath, NULL);

//    av_log_set_level(AV_LOG_DEBUG); // 设置FFmpeg日志级别为DEBUG

    av_register_all();
    if ((ret = avformat_open_input(&ifmt_ctx, h264File, 0, 0)) < 0) {
        ALOGV( "FFmpegApi_convertH264ToMp4File Could not open input file '%s'", h264File);
        goto end;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        ALOGV( "FFmpegApi_convertH264ToMp4File Failed to retrieve input stream information");
        goto end;
    }


    av_dump_format(ifmt_ctx, 0, h264File, 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, mp4File);
    if (!ofmt_ctx) {
        ALOGV("FFmpegApi_convertH264ToMp4File Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    stream_mapping_size = ifmt_ctx->nb_streams;
    stream_mapping = (int *)av_mallocz_array(stream_mapping_size, sizeof(*stream_mapping));
    if (!stream_mapping) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *out_stream;
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }

        if (in_codecpar->codec_type==AVMEDIA_TYPE_AUDIO)
        {
            stream_index_audio=i;
        }
        else if (in_codecpar->codec_type==AVMEDIA_TYPE_VIDEO)
        {
            stream_index_video=i;
        }


        stream_mapping[i] = stream_index++;

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            ALOGV("FFmpegApi_convertH264ToMp4File Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            ALOGV("FFmpegApi_convertH264ToMp4File Failed to copy codec parameters\n");
            goto end;
        }
        out_stream->codecpar->codec_tag = 0;

        if (in_codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            AVRational frame_rate = av_guess_frame_rate(ifmt_ctx, in_stream, NULL);
            out_stream->time_base = av_inv_q(frame_rate);
            ALOGV("FFmpegApi_convertH264ToMp4File %d, %d\n",out_stream->time_base.den,out_stream->time_base.num);
            out_stream->time_base.num=1;
        }

    }
    av_dump_format(ofmt_ctx, 0, mp4File, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, mp4File, AVIO_FLAG_WRITE);
        if (ret < 0) {
            ALOGV( "FFmpegApi_convertH264ToMp4File Could not open output file '%s'", mp4File);
            goto end;
        }
    }

    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        ALOGV( "FFmpegApi_convertH264ToMp4File Error occurred when opening output file\n");
        goto end;
    }

    bool audio_bFirst=true;
    int64_t audio_gpts=0,audio_gdts=0,audio_gduration=0;
    bool video_bFirst=true;
    int64_t video_gpts=0,video_gdts=0,video_gduration=0;

    int64_t llFrameIndex=0;

    while (1) {

        memset(&pkt,0,sizeof(AVPacket));
        pkt.stream_index=2;
        AVStream *in_stream, *out_stream;

        ret = av_read_frame(ifmt_ctx, &pkt);
        if (ret < 0)
            break;
        if (pkt.stream_index != stream_index_video)
            continue;

        in_stream  = ifmt_ctx->streams[pkt.stream_index];
        if (pkt.stream_index >= stream_mapping_size ||
            stream_mapping[pkt.stream_index] < 0) {
            av_packet_unref(&pkt);
            continue;
        }

        pkt.stream_index = stream_mapping[pkt.stream_index];
        out_stream = ofmt_ctx->streams[pkt.stream_index];
        if (pkt.stream_index==0)
            ALOGV(ifmt_ctx, &pkt, "in");

        if (pkt.pts<pkt.dts ||pkt.duration<0 )
            continue;

        if (pkt.pts ==AV_NOPTS_VALUE || pkt.dts ==AV_NOPTS_VALUE)
        {

            AVRational time_base1 = in_stream->time_base;
            int64_t calc_duration = (double)AV_TIME_BASE /av_q2d(in_stream->r_frame_rate);

            pkt.pts = (double) (llFrameIndex * calc_duration);
            pkt.dts = pkt.pts;

            pkt.duration =(double)calc_duration /(double)(av_q2d(time_base1)*AV_TIME_BASE);
            llFrameIndex++;

            //continue;
        }
        /* copy packet */
        pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX);
        pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
        ALOGV("FFmpegApi_convertH264ToMp4File video process pts:%ld ,duration:%ld\n",pkt.pts ,pkt.duration);
        pkt.pos = -1;
        if (pkt.stream_index>=ifmt_ctx->nb_streams)
        {
            ALOGV("stream_index \n");
        }

        if (pkt.stream_index==stream_index_video)
        {
            ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
            //if (ret<0 && ret!=-22)
            if (ret < 0)
            {
                ALOGV( "FFmpegApi_h264ToMP4Converter Error muxing packet\n");
                break;
            }
        }
        av_packet_unref(&pkt);
    }

    av_write_trailer(ofmt_ctx);
    end:

    avformat_close_input(&ifmt_ctx);

    /* close output */
    if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    av_freep(&stream_mapping);

    if (ret < 0 && ret != AVERROR_EOF) {
        ALOGV("FFmpegApi_h264ToMP4Converter Error occurred: %s\n", av_err2str(ret));
        return JNI_FALSE;
    }

    ALOGV("FFmpegApi_h264ToMP4Converter end");
    return JNI_TRUE;
}


static jobject
FFmpegApi_getVideoInfo(JNIEnv *env, jclass clazz, jstring inputPath) {
    const char *input_path = (*env)->GetStringUTFChars(env, inputPath, NULL);

    // 注册所有格式和编解码器
    av_register_all();

    // 打开输入文件并读取头部
    AVFormatContext *format_ctx = avformat_alloc_context();
    if (avformat_open_input(&format_ctx, input_path, NULL, NULL) != 0) {
        // 打开输入文件失败
        (*env)->ReleaseStringUTFChars(env, inputPath, input_path);
        return NULL;
    }

    // 查找流信息
    if (avformat_find_stream_info(format_ctx, NULL) < 0) {
        avformat_close_input(&format_ctx);
        (*env)->ReleaseStringUTFChars(env, inputPath, input_path);
        return NULL;
    }

    // 查找视频流
    int video_stream_index = -1;
    for (int i = 0; i < format_ctx->nb_streams; i++) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index = i;
            break;
        }
    }

    if (video_stream_index == -1) {
        avformat_close_input(&format_ctx);
        (*env)->ReleaseStringUTFChars(env, inputPath, input_path);
        return NULL;
    }

    AVStream *video_stream = format_ctx->streams[video_stream_index];
    AVCodecParameters *codec_par = video_stream->codecpar;

    // 获取帧率
    double frame_rate = av_q2d(video_stream->avg_frame_rate);

    // 获取码率
    long bit_rate = codec_par->bit_rate/1000;

    // 获取编码格式
    const char *codec_name = avcodec_get_name(codec_par->codec_id);

    // 查找 Java 的 VideoInfo 类和构造方法
    jclass video_info_class = (*env)->FindClass(env, "tv/danmaku/ijk/media/player/ffmpeg/FFmpegApi$VideoInfo");
    jmethodID constructor = (*env)->GetMethodID(env, video_info_class, "<init>", "(DJLjava/lang/String;)V");

    // 创建 VideoInfo 对象并返回
    jstring codec_name_str = (*env)->NewStringUTF(env, codec_name);
    jobject video_info_obj = (*env)->NewObject(env, video_info_class, constructor, frame_rate, bit_rate, codec_name_str);

    // 释放资源
    avformat_close_input(&format_ctx);
    (*env)->ReleaseStringUTFChars(env, inputPath, input_path);

    return video_info_obj;
}

static JNINativeMethod g_methods[] = {
        {"av_base64_encode", "([B)Ljava/lang/String;", (void *) FFmpegApi_av_base64_encode},
        {"h264ToMP4Converter", "(Ljava/lang/String;Ljava/lang/String;)Z", (void *) FFmpegApi_h264ToMP4Converter},
        {"convertH264ToMp4File", "(Ljava/lang/String;Ljava/lang/String;)Z", (void *) FFmpegApi_convertH264ToMp4File},
        {"getVideoInfo", "(Ljava/lang/String;)Ltv/danmaku/ijk/media/player/ffmpeg/FFmpegApi$VideoInfo;", (void *) FFmpegApi_getVideoInfo},
};


int FFmpegApi_global_init(JNIEnv *env)
{
    int ret = 0;

    IJK_FIND_JAVA_CLASS(env, g_clazz.clazz, JNI_CLASS_FFMPEG_API)
    (*env)->RegisterNatives(env, g_clazz.clazz, g_methods, NELEM(g_methods));

    return ret;
}

