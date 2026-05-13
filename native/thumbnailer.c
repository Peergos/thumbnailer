#include <jni.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

/*
 * Seeks to ~1 second into the video (or as close as possible), decodes one
 * frame, scales it so the longest edge == maxSize (aspect-ratio preserved),
 * encodes as JPEG, and returns the bytes to Java.
 *
 * Returns NULL on any error.
 */
JNIEXPORT jbyteArray JNICALL
Java_org_peergos_thumbnailer_VideoThumbnailer_generate(
        JNIEnv *env, jclass cls, jstring jpath, jint maxSize)
{
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);

    AVFormatContext  *fmt_ctx   = NULL;
    AVCodecContext   *dec_ctx   = NULL;
    AVFrame          *src_frame = NULL;
    AVFrame          *dst_frame = NULL;
    AVPacket         *pkt       = NULL;
    struct SwsContext *sws      = NULL;
    AVCodecContext   *enc_ctx   = NULL;
    AVPacket         *out_pkt   = NULL;
    jbyteArray        result    = NULL;

    /* ── open container ── */
    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0)
        goto cleanup;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
        goto cleanup;

    /* ── find best video stream ── */
    int vsi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vsi < 0)
        goto cleanup;
    AVStream *stream = fmt_ctx->streams[vsi];

    /* ── open decoder ── */
    const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec)
        goto cleanup;
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        goto cleanup;
    if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0)
        goto cleanup;
    dec_ctx->thread_count = 1; /* single-threaded is fine for one frame */
    if (avcodec_open2(dec_ctx, dec, NULL) < 0)
        goto cleanup;

    /* ── seek to 1 s (best-effort; ignored for short videos) ── */
    int64_t one_sec = av_rescale_q(AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);
    av_seek_frame(fmt_ctx, vsi, one_sec, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    /* ── decode one frame ── */
    src_frame = av_frame_alloc();
    pkt       = av_packet_alloc();
    if (!src_frame || !pkt)
        goto cleanup;

    int got = 0;
    for (int tries = 0; tries < 300 && !got; tries++) {
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0)
            break;
        if (pkt->stream_index == vsi) {
            if (avcodec_send_packet(dec_ctx, pkt) >= 0)
                got = (avcodec_receive_frame(dec_ctx, src_frame) >= 0);
        }
        av_packet_unref(pkt);
    }
    if (!got)
        goto cleanup;

    /* ── scale: longest edge == maxSize, keep aspect ratio, even dims ── */
    int sw = src_frame->width, sh = src_frame->height;
    int dw, dh;
    if (sw >= sh) {
        dw = maxSize;
        dh = (int)((double)sh / sw * maxSize);
    } else {
        dh = maxSize;
        dw = (int)((double)sw / sh * maxSize);
    }
    dw = (dw + 1) & ~1;
    dh = (dh + 1) & ~1;

    sws = sws_getContext(sw, sh, src_frame->format,
                         dw, dh, AV_PIX_FMT_RGB24,
                         SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws)
        goto cleanup;

    dst_frame = av_frame_alloc();
    if (!dst_frame)
        goto cleanup;
    dst_frame->format = AV_PIX_FMT_RGB24;
    dst_frame->width  = dw;
    dst_frame->height = dh;
    if (av_image_alloc(dst_frame->data, dst_frame->linesize,
                       dw, dh, AV_PIX_FMT_RGB24, 32) < 0)
        goto cleanup;

    sws_scale(sws,
              (const uint8_t * const *)src_frame->data, src_frame->linesize,
              0, sh,
              dst_frame->data, dst_frame->linesize);

    /* ── encode to WebP via libwebp ── */
    const AVCodec *enc = avcodec_find_encoder_by_name("libwebp");
    if (!enc)
        goto cleanup;
    enc_ctx = avcodec_alloc_context3(enc);
    if (!enc_ctx)
        goto cleanup;
    enc_ctx->width     = dw;
    enc_ctx->height    = dh;
    enc_ctx->pix_fmt   = AV_PIX_FMT_RGB24;
    enc_ctx->time_base = (AVRational){1, 25};
    if (avcodec_open2(enc_ctx, enc, NULL) < 0)
        goto cleanup;
    /* match libwebp's default quality (WebPConfigInit sets 75) */
    av_opt_set_int(enc_ctx->priv_data, "quality", 75, 0);

    out_pkt = av_packet_alloc();
    if (!out_pkt)
        goto cleanup;
    if (avcodec_send_frame(enc_ctx, dst_frame) >= 0 &&
        avcodec_receive_packet(enc_ctx, out_pkt) >= 0) {
        result = (*env)->NewByteArray(env, out_pkt->size);
        if (result)
            (*env)->SetByteArrayRegion(env, result, 0, out_pkt->size,
                                       (const jbyte *)out_pkt->data);
    }

cleanup:
    if (out_pkt)  av_packet_free(&out_pkt);
    if (enc_ctx)  avcodec_free_context(&enc_ctx);
    if (dst_frame) { av_freep(&dst_frame->data[0]); av_frame_free(&dst_frame); }
    if (sws)      sws_freeContext(sws);
    if (pkt)      av_packet_free(&pkt);
    if (src_frame) av_frame_free(&src_frame);
    if (dec_ctx)  avcodec_free_context(&dec_ctx);
    if (fmt_ctx)  avformat_close_input(&fmt_ctx);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return result;
}
