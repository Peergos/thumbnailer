#include <jni.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

/* Encode dst_frame (YUV420P) as WebP quality 75 and return a new jbyteArray.
   Frees dst_frame (and its data) on both success and failure. */
static jbyteArray webp_encode_frame(JNIEnv *env, AVFrame *dst_frame)
{
    AVCodecContext *enc_ctx = NULL;
    AVPacket       *out_pkt = NULL;
    jbyteArray      result  = NULL;

    const AVCodec *enc = avcodec_find_encoder_by_name("libwebp");
    if (!enc) goto done;
    enc_ctx = avcodec_alloc_context3(enc);
    if (!enc_ctx) goto done;
    enc_ctx->width     = dst_frame->width;
    enc_ctx->height    = dst_frame->height;
    enc_ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    enc_ctx->time_base = (AVRational){1, 25};
    if (avcodec_open2(enc_ctx, enc, NULL) < 0) goto done;
    av_opt_set_int(enc_ctx->priv_data, "quality", 75, 0);

    out_pkt = av_packet_alloc();
    if (!out_pkt) goto done;
    if (avcodec_send_frame(enc_ctx, dst_frame) >= 0 &&
        avcodec_receive_packet(enc_ctx, out_pkt) >= 0) {
        result = (*env)->NewByteArray(env, out_pkt->size);
        if (result)
            (*env)->SetByteArrayRegion(env, result, 0, out_pkt->size,
                                       (const jbyte *)out_pkt->data);
    }

done:
    if (out_pkt) av_packet_free(&out_pkt);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    av_freep(&dst_frame->data[0]);
    av_frame_free(&dst_frame);
    return result;
}

/*
 * Scale src_frame so the longest edge == maxSize (aspect-ratio preserved,
 * even dims), encode as WebP quality 75, return a new jbyteArray or NULL.
 * Used for video thumbnails.
 */
static jbyteArray encode_webp(JNIEnv *env, AVFrame *src_frame, int maxSize)
{
    int sw = src_frame->width, sh = src_frame->height;
    int dw, dh;
    if (sw >= sh) { dw = maxSize; dh = (int)((double)sh / sw * maxSize); }
    else          { dh = maxSize; dw = (int)((double)sw / sh * maxSize); }
    dw = (dw + 1) & ~1;
    dh = (dh + 1) & ~1;

    struct SwsContext *sws = sws_getContext(sw, sh, src_frame->format,
                                            dw, dh, AV_PIX_FMT_YUV420P,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return NULL;

    AVFrame *dst = av_frame_alloc();
    if (!dst) { sws_freeContext(sws); return NULL; }
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = dw;
    dst->height = dh;
    if (av_image_alloc(dst->data, dst->linesize, dw, dh, AV_PIX_FMT_YUV420P, 32) < 0) {
        sws_freeContext(sws); av_frame_free(&dst); return NULL;
    }
    sws_scale(sws, (const uint8_t * const *)src_frame->data, src_frame->linesize,
              0, sh, dst->data, dst->linesize);
    sws_freeContext(sws);

    return webp_encode_frame(env, dst);
}

/*
 * Scale src_frame so the SHORT edge == size, center-crop to size x size,
 * encode as WebP quality 75, return a new jbyteArray or NULL.
 * Matches JavaImageThumbnailer's behaviour exactly.
 */
static jbyteArray encode_webp_square(JNIEnv *env, AVFrame *src_frame, int size)
{
    int sw = src_frame->width, sh = src_frame->height;

    /* scale so the short edge == size */
    int cw, ch;
    if (sh > sw) { cw = size; ch = (int)((double)sh / sw * size); }
    else         { ch = size; cw = (int)((double)sw / sh * size); }
    /* keep even for YUV420P subsampling */
    cw = (cw + 1) & ~1;
    ch = (ch + 1) & ~1;

    struct SwsContext *sws = sws_getContext(sw, sh, src_frame->format,
                                            cw, ch, AV_PIX_FMT_YUV420P,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return NULL;

    uint8_t *canvas[4] = {0};
    int      linesize[4] = {0};
    if (av_image_alloc(canvas, linesize, cw, ch, AV_PIX_FMT_YUV420P, 32) < 0) {
        sws_freeContext(sws); return NULL;
    }
    sws_scale(sws, (const uint8_t * const *)src_frame->data, src_frame->linesize,
              0, sh, canvas, linesize);
    sws_freeContext(sws);

    /* center-crop offsets — must be even for correct UV alignment */
    int x_off = ((cw - size) / 2) & ~1;
    int y_off = ((ch - size) / 2) & ~1;

    AVFrame *dst = av_frame_alloc();
    if (!dst) { av_freep(&canvas[0]); return NULL; }
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = size;
    dst->height = size;
    if (av_image_alloc(dst->data, dst->linesize, size, size, AV_PIX_FMT_YUV420P, 32) < 0) {
        av_freep(&canvas[0]); av_frame_free(&dst); return NULL;
    }

    /* Y plane: 1 byte/pixel, full resolution */
    for (int row = 0; row < size; row++)
        memcpy(dst->data[0] + row * dst->linesize[0],
               canvas[0] + (y_off + row) * linesize[0] + x_off,
               size);

    /* U and V planes: 1 byte per 2x2 block */
    for (int row = 0; row < size / 2; row++) {
        memcpy(dst->data[1] + row * dst->linesize[1],
               canvas[1] + (y_off / 2 + row) * linesize[1] + x_off / 2,
               size / 2);
        memcpy(dst->data[2] + row * dst->linesize[2],
               canvas[2] + (y_off / 2 + row) * linesize[2] + x_off / 2,
               size / 2);
    }
    av_freep(&canvas[0]);

    return webp_encode_frame(env, dst);
}

/*
 * Seeks to ~1 second into the video (or as close as possible), decodes one
 * frame, scales it so the longest edge == maxSize (aspect-ratio preserved),
 * encodes as WebP, and returns the bytes to Java.
 *
 * Returns NULL on any error.
 */
JNIEXPORT jbyteArray JNICALL
Java_org_peergos_thumbnailer_VideoThumbnailer_generate(
        JNIEnv *env, jclass cls, jstring jpath, jint maxSize)
{
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);

    AVFormatContext *fmt_ctx   = NULL;
    AVCodecContext  *dec_ctx   = NULL;
    AVFrame         *src_frame = NULL;
    AVPacket        *pkt       = NULL;
    jbyteArray       result    = NULL;

    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) goto cleanup;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) goto cleanup;

    int vsi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vsi < 0) goto cleanup;
    AVStream *stream = fmt_ctx->streams[vsi];

    const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) goto cleanup;
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) goto cleanup;
    if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0) goto cleanup;
    dec_ctx->thread_count = 1;
    if (avcodec_open2(dec_ctx, dec, NULL) < 0) goto cleanup;

    /* seek to 1 s (best-effort; ignored for short videos) */
    int64_t one_sec = av_rescale_q(AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);
    av_seek_frame(fmt_ctx, vsi, one_sec, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_ctx);

    src_frame = av_frame_alloc();
    pkt       = av_packet_alloc();
    if (!src_frame || !pkt) goto cleanup;

    int got = 0;
    for (int tries = 0; tries < 300 && !got; tries++) {
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) break;
        if (pkt->stream_index == vsi) {
            if (avcodec_send_packet(dec_ctx, pkt) >= 0)
                got = (avcodec_receive_frame(dec_ctx, src_frame) >= 0);
        }
        av_packet_unref(pkt);
    }
    if (!got) goto cleanup;

    result = encode_webp(env, src_frame, (int)maxSize);

cleanup:
    if (pkt)       av_packet_free(&pkt);
    if (src_frame) av_frame_free(&src_frame);
    if (dec_ctx)   avcodec_free_context(&dec_ctx);
    if (fmt_ctx)   avformat_close_input(&fmt_ctx);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return result;
}

/*
 * Decodes the first frame of an image file (JPEG, PNG, GIF, BMP, WebP, TIFF,
 * AVIF…), scales so the short edge == size, center-crops to size x size,
 * encodes as WebP quality 75.  Matches JavaImageThumbnailer output exactly.
 *
 * Returns NULL on any error.
 */
JNIEXPORT jbyteArray JNICALL
Java_org_peergos_thumbnailer_VideoThumbnailer_generateImage(
        JNIEnv *env, jclass cls, jstring jpath, jint maxSize)
{
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);

    AVFormatContext *fmt_ctx   = NULL;
    AVCodecContext  *dec_ctx   = NULL;
    AVFrame         *src_frame = NULL;
    AVPacket        *pkt       = NULL;
    jbyteArray       result    = NULL;

    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) goto cleanup;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) goto cleanup;

    int vsi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vsi < 0) goto cleanup;
    AVStream *stream = fmt_ctx->streams[vsi];

    const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) goto cleanup;
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) goto cleanup;
    if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0) goto cleanup;
    dec_ctx->thread_count = 1;
    if (avcodec_open2(dec_ctx, dec, NULL) < 0) goto cleanup;

    src_frame = av_frame_alloc();
    pkt       = av_packet_alloc();
    if (!src_frame || !pkt) goto cleanup;

    int got = 0;
    for (int tries = 0; tries < 300 && !got; tries++) {
        int ret = av_read_frame(fmt_ctx, pkt);
        if (ret < 0) break;
        if (pkt->stream_index == vsi) {
            if (avcodec_send_packet(dec_ctx, pkt) >= 0)
                got = (avcodec_receive_frame(dec_ctx, src_frame) >= 0);
        }
        av_packet_unref(pkt);
    }
    if (!got) goto cleanup;

    result = encode_webp_square(env, src_frame, (int)maxSize);

cleanup:
    if (pkt)       av_packet_free(&pkt);
    if (src_frame) av_frame_free(&src_frame);
    if (dec_ctx)   avcodec_free_context(&dec_ctx);
    if (fmt_ctx)   avformat_close_input(&fmt_ctx);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return result;
}
