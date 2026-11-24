#include <jni.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#include <stdlib.h>
#include <string.h>

static void log_callback(void *ptr, int level, const char *fmt, va_list vl)
{
    if (level > av_log_get_level()) return;
    if (strstr(fmt, "deprecated pixel format")) return;
    if (strstr(fmt, "invalid TIFF header in Exif")) return;
    if (strstr(fmt, "skipping unsupported chunk")) return;
    av_log_default_callback(ptr, level, fmt, vl);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    av_log_set_callback(log_callback);
    return JNI_VERSION_1_6;
}

/* ── orientation helpers ──────────────────────────────────────────────────── */

/*
 * Read the EXIF orientation (1–8) from the stream's display matrix side data,
 * or, for a JPEG without one, from the decoded frame's, where ffmpeg's mjpeg
 * decoder puts a JPEG's EXIF orientation. Other codecs' frame matrices are
 * ignored: browsers ignore WebP EXIF, and ffmpeg can't tell a PNG eXIf before
 * IDAT (which browsers apply) from one after it (which they ignore).
 * Returns 1 (normal) if no matrix is found.
 * The 2×2 top-left sign pattern maps unambiguously to each orientation.
 */
static int get_exif_orientation(AVStream *stream, const AVFrame *frame)
{
    const AVPacketSideData *sd = av_packet_side_data_get(
        stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    const AVFrameSideData *fsd = !sd && stream->codecpar->codec_id == AV_CODEC_ID_MJPEG ?
        av_frame_get_side_data(frame, AV_FRAME_DATA_DISPLAYMATRIX) : NULL;
    if (!sd && !fsd) return 1;

    const int32_t *m = (const int32_t *)(sd ? sd->data : fsd->data);
    int a = m[0] > 0 ?  1 : m[0] < 0 ? -1 : 0;
    int b = m[1] > 0 ?  1 : m[1] < 0 ? -1 : 0;
    int c = m[3] > 0 ?  1 : m[3] < 0 ? -1 : 0;
    int d = m[4] > 0 ?  1 : m[4] < 0 ? -1 : 0;

    if (a== 1 && b== 0 && c== 0 && d== 1) return 1; /* normal        */
    if (a==-1 && b== 0 && c== 0 && d== 1) return 2; /* flip-H        */
    if (a==-1 && b== 0 && c== 0 && d==-1) return 3; /* rotate 180    */
    if (a== 1 && b== 0 && c== 0 && d==-1) return 4; /* flip-V        */
    if (a== 0 && b== 1 && c== 1 && d== 0) return 5; /* transpose     */
    if (a== 0 && b== 1 && c==-1 && d== 0) return 6; /* rotate 90 CW  */
    if (a== 0 && b==-1 && c==-1 && d== 0) return 7; /* transverse    */
    if (a== 0 && b==-1 && c== 1 && d== 0) return 8; /* rotate 270 CW */
    return 1;
}

/*
 * Read the clockwise display rotation (0/90/180/270) for videos from the
 * stream's display matrix.  Uses av_display_rotation_get which is reliable
 * for pure rotations (no flips) as found in video containers.
 */
static int get_video_rotation(AVStream *stream)
{
    const AVPacketSideData *sd = av_packet_side_data_get(
        stream->codecpar->coded_side_data,
        stream->codecpar->nb_coded_side_data,
        AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd) return 0;
    double theta = -av_display_rotation_get((const int32_t *)sd->data);
    if (theta < 0) theta += 360;
    if (theta >  45 && theta <= 135) return 90;
    if (theta > 135 && theta <= 225) return 180;
    if (theta > 225 && theta <= 315) return 270;
    return 0;
}

/* ── YUV420P frame transforms ─────────────────────────────────────────────── */
/* Each function takes ownership of src (frees it) and returns a new frame. */

static AVFrame *rotate_90cw(AVFrame *src)
{
    int sw = src->width, sh = src->height;
    AVFrame *dst = av_frame_alloc();
    if (!dst) return NULL;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = sh; dst->height = sw;
    if (av_image_alloc(dst->data, dst->linesize, sh, sw, AV_PIX_FMT_YUV420P, 32) < 0)
        { av_frame_free(&dst); return NULL; }
    for (int y = 0; y < sw; y++)
        for (int x = 0; x < sh; x++)
            dst->data[0][y * dst->linesize[0] + x] =
                src->data[0][(sh - 1 - x) * src->linesize[0] + y];
    for (int y = 0; y < sw/2; y++)
        for (int x = 0; x < sh/2; x++) {
            dst->data[1][y * dst->linesize[1] + x] =
                src->data[1][(sh/2 - 1 - x) * src->linesize[1] + y];
            dst->data[2][y * dst->linesize[2] + x] =
                src->data[2][(sh/2 - 1 - x) * src->linesize[2] + y];
        }
    av_freep(&src->data[0]); av_frame_free(&src);
    return dst;
}

static AVFrame *rotate_90ccw(AVFrame *src)
{
    int sw = src->width, sh = src->height;
    AVFrame *dst = av_frame_alloc();
    if (!dst) return NULL;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = sh; dst->height = sw;
    if (av_image_alloc(dst->data, dst->linesize, sh, sw, AV_PIX_FMT_YUV420P, 32) < 0)
        { av_frame_free(&dst); return NULL; }
    for (int y = 0; y < sw; y++)
        for (int x = 0; x < sh; x++)
            dst->data[0][y * dst->linesize[0] + x] =
                src->data[0][x * src->linesize[0] + (sw - 1 - y)];
    for (int y = 0; y < sw/2; y++)
        for (int x = 0; x < sh/2; x++) {
            dst->data[1][y * dst->linesize[1] + x] =
                src->data[1][x * src->linesize[1] + (sw/2 - 1 - y)];
            dst->data[2][y * dst->linesize[2] + x] =
                src->data[2][x * src->linesize[2] + (sw/2 - 1 - y)];
        }
    av_freep(&src->data[0]); av_frame_free(&src);
    return dst;
}

static AVFrame *rotate_180(AVFrame *src)
{
    int sw = src->width, sh = src->height;
    AVFrame *dst = av_frame_alloc();
    if (!dst) return NULL;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = sw; dst->height = sh;
    if (av_image_alloc(dst->data, dst->linesize, sw, sh, AV_PIX_FMT_YUV420P, 32) < 0)
        { av_frame_free(&dst); return NULL; }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            dst->data[0][y * dst->linesize[0] + x] =
                src->data[0][(sh - 1 - y) * src->linesize[0] + (sw - 1 - x)];
    for (int y = 0; y < sh/2; y++)
        for (int x = 0; x < sw/2; x++) {
            dst->data[1][y * dst->linesize[1] + x] =
                src->data[1][(sh/2 - 1 - y) * src->linesize[1] + (sw/2 - 1 - x)];
            dst->data[2][y * dst->linesize[2] + x] =
                src->data[2][(sh/2 - 1 - y) * src->linesize[2] + (sw/2 - 1 - x)];
        }
    av_freep(&src->data[0]); av_frame_free(&src);
    return dst;
}

static AVFrame *flip_horizontal(AVFrame *src)
{
    int sw = src->width, sh = src->height;
    AVFrame *dst = av_frame_alloc();
    if (!dst) return NULL;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = sw; dst->height = sh;
    if (av_image_alloc(dst->data, dst->linesize, sw, sh, AV_PIX_FMT_YUV420P, 32) < 0)
        { av_frame_free(&dst); return NULL; }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            dst->data[0][y * dst->linesize[0] + x] =
                src->data[0][y * src->linesize[0] + (sw - 1 - x)];
    for (int y = 0; y < sh/2; y++)
        for (int x = 0; x < sw/2; x++) {
            dst->data[1][y * dst->linesize[1] + x] =
                src->data[1][y * src->linesize[1] + (sw/2 - 1 - x)];
            dst->data[2][y * dst->linesize[2] + x] =
                src->data[2][y * src->linesize[2] + (sw/2 - 1 - x)];
        }
    av_freep(&src->data[0]); av_frame_free(&src);
    return dst;
}

static AVFrame *flip_vertical(AVFrame *src)
{
    int sw = src->width, sh = src->height;
    AVFrame *dst = av_frame_alloc();
    if (!dst) return NULL;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = sw; dst->height = sh;
    if (av_image_alloc(dst->data, dst->linesize, sw, sh, AV_PIX_FMT_YUV420P, 32) < 0)
        { av_frame_free(&dst); return NULL; }
    for (int y = 0; y < sh; y++)
        memcpy(dst->data[0] + y * dst->linesize[0],
               src->data[0] + (sh - 1 - y) * src->linesize[0], sw);
    for (int y = 0; y < sh/2; y++) {
        memcpy(dst->data[1] + y * dst->linesize[1],
               src->data[1] + (sh/2 - 1 - y) * src->linesize[1], sw/2);
        memcpy(dst->data[2] + y * dst->linesize[2],
               src->data[2] + (sh/2 - 1 - y) * src->linesize[2], sw/2);
    }
    av_freep(&src->data[0]); av_frame_free(&src);
    return dst;
}

/*
 * Apply EXIF orientation 1–8 to a YUV420P frame.
 * Matches AndroidImageThumbnailer.rotatedImage() exactly.
 * Takes ownership of frame; returns new frame or NULL on alloc failure.
 */
static AVFrame *apply_exif_orientation(AVFrame *frame, int orientation)
{
    switch (orientation) {
        case 2: return flip_horizontal(frame);
        case 3: return rotate_180(frame);
        case 4: return flip_vertical(frame);
        case 5: { AVFrame *t = flip_horizontal(frame); return t ? rotate_90ccw(t) : NULL; }
        case 6: return rotate_90cw(frame);
        case 7: { AVFrame *t = flip_horizontal(frame); return t ? rotate_90cw(t)  : NULL; }
        case 8: return rotate_90ccw(frame);
        default: return frame; /* 1 = normal */
    }
}

/* ── WebP encoder ─────────────────────────────────────────────────────────── */

/* Encode dst_frame (YUV420P) as WebP at the given quality.
   Takes ownership of dst_frame. */
static jbyteArray webp_encode_frame(JNIEnv *env, AVFrame *dst_frame, int quality)
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
    av_opt_set_int(enc_ctx->priv_data, "lossless", 0, 0);
    av_opt_set_int(enc_ctx->priv_data, "quality",  quality, 0);
    if (avcodec_open2(enc_ctx, enc, NULL) < 0) goto done;

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
    if (out_pkt)   av_packet_free(&out_pkt);
    if (enc_ctx)   avcodec_free_context(&enc_ctx);
    av_freep(&dst_frame->data[0]);
    av_frame_free(&dst_frame);
    return result;
}

/* ── thumbnail producers ──────────────────────────────────────────────────── */

/*
 * Scale src_frame to fit maxSize on the longest displayed edge (honouring
 * rotation), rotate to the correct orientation, encode as WebP quality 75.
 * Used for video thumbnails.
 */
static jbyteArray encode_webp_video(JNIEnv *env, AVFrame *src_frame,
                                    int maxSize, int rotation)
{
    int sw = src_frame->width, sh = src_frame->height;

    int ew = (rotation == 90 || rotation == 270) ? sh : sw;
    int eh = (rotation == 90 || rotation == 270) ? sw : sh;

    int dw, dh;
    if (ew >= eh) { dw = maxSize; dh = (int)((double)eh / ew * maxSize); }
    else          { dh = maxSize; dw = (int)((double)ew / eh * maxSize); }
    dw = (dw + 1) & ~1;
    dh = (dh + 1) & ~1;

    int scale_w = (rotation == 90 || rotation == 270) ? dh : dw;
    int scale_h = (rotation == 90 || rotation == 270) ? dw : dh;

    struct SwsContext *sws = sws_getContext(sw, sh, src_frame->format,
                                            scale_w, scale_h, AV_PIX_FMT_YUV420P,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return NULL;

    AVFrame *scaled = av_frame_alloc();
    if (!scaled) { sws_freeContext(sws); return NULL; }
    scaled->format = AV_PIX_FMT_YUV420P;
    scaled->width  = scale_w; scaled->height = scale_h;
    if (av_image_alloc(scaled->data, scaled->linesize,
                       scale_w, scale_h, AV_PIX_FMT_YUV420P, 32) < 0)
        { sws_freeContext(sws); av_frame_free(&scaled); return NULL; }
    sws_scale(sws, (const uint8_t * const *)src_frame->data, src_frame->linesize,
              0, sh, scaled->data, scaled->linesize);
    sws_freeContext(sws);

    AVFrame *oriented;
    switch (rotation) {
        case 90:  oriented = rotate_90cw(scaled);  break;
        case 180: oriented = rotate_180(scaled);    break;
        case 270: oriented = rotate_90ccw(scaled);  break;
        default:  oriented = scaled;                break;
    }
    if (!oriented) return NULL;
    return webp_encode_frame(env, oriented, 75);
}

/*
 * Center-crop src_frame to a square of side `size`, apply EXIF orientation,
 * encode as WebP quality 100.
 * Matches AndroidImageThumbnailer exactly.
 */
static jbyteArray encode_webp_image(JNIEnv *env, AVFrame *src_frame,
                                    int size, int orientation)
{
    int sw = src_frame->width, sh = src_frame->height;

    /* scale so short edge == size */
    int cw, ch;
    if (sh > sw) { cw = size; ch = (int)((double)sh / sw * size); }
    else         { ch = size; cw = (int)((double)sw / sh * size); }
    cw = (cw + 1) & ~1;
    ch = (ch + 1) & ~1;

    struct SwsContext *sws = sws_getContext(sw, sh, src_frame->format,
                                            cw, ch, AV_PIX_FMT_YUV420P,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return NULL;

    uint8_t *canvas[4] = {0};
    int      linesize[4] = {0};
    if (av_image_alloc(canvas, linesize, cw, ch, AV_PIX_FMT_YUV420P, 32) < 0)
        { sws_freeContext(sws); return NULL; }
    sws_scale(sws, (const uint8_t * const *)src_frame->data, src_frame->linesize,
              0, sh, canvas, linesize);
    sws_freeContext(sws);

    /* center crop to size × size */
    int x_off = ((cw - size) / 2) & ~1;
    int y_off = ((ch - size) / 2) & ~1;

    AVFrame *dst = av_frame_alloc();
    if (!dst) { av_freep(&canvas[0]); return NULL; }
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = size; dst->height = size;
    if (av_image_alloc(dst->data, dst->linesize, size, size, AV_PIX_FMT_YUV420P, 32) < 0)
        { av_freep(&canvas[0]); av_frame_free(&dst); return NULL; }

    for (int row = 0; row < size; row++)
        memcpy(dst->data[0] + row * dst->linesize[0],
               canvas[0] + (y_off + row) * linesize[0] + x_off, size);
    for (int row = 0; row < size / 2; row++) {
        memcpy(dst->data[1] + row * dst->linesize[1],
               canvas[1] + (y_off/2 + row) * linesize[1] + x_off/2, size/2);
        memcpy(dst->data[2] + row * dst->linesize[2],
               canvas[2] + (y_off/2 + row) * linesize[2] + x_off/2, size/2);
    }
    av_freep(&canvas[0]);

    /* apply EXIF orientation (same logic as AndroidImageThumbnailer) */
    AVFrame *oriented = apply_exif_orientation(dst, orientation);
    if (!oriented) return NULL;
    jbyteArray result = webp_encode_frame(env, oriented, 100);

    /* If > 100 KiB, retry at half size — matches Android's tooBig catch */
    if (result != NULL && (*env)->GetArrayLength(env, result) > 100 * 1024) {
        (*env)->DeleteLocalRef(env, result);
        result = encode_webp_image(env, src_frame, size / 2, orientation);
    }
    return result;
}

/* ── video frame choice ───────────────────────────────────────────────────── */

/* A frame is blank when 80% or more of it is near black or near white, which is the
   test the web client uses: a title card or a fade is not worth a thumbnail. Sampled
   on a small greyscale copy, so the cost does not depend on the video's resolution. */
#define BLANK_SAMPLE 32
#define BLANK_BLACK  7
#define BLANK_WHITE  248

static int frame_is_blank(AVFrame *frame)
{
    struct SwsContext *sws = sws_getContext(frame->width, frame->height, frame->format,
                                            BLANK_SAMPLE, BLANK_SAMPLE, AV_PIX_FMT_GRAY8,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return 0;

    uint8_t *grey = av_malloc(BLANK_SAMPLE * BLANK_SAMPLE);
    if (!grey) { sws_freeContext(sws); return 0; }
    uint8_t *dst[4] = { grey, NULL, NULL, NULL };
    int dst_linesize[4] = { BLANK_SAMPLE, 0, 0, 0 };
    int scaled = sws_scale(sws, (const uint8_t * const *)frame->data, frame->linesize,
                           0, frame->height, dst, dst_linesize);
    sws_freeContext(sws);
    if (scaled <= 0) { av_free(grey); return 0; }

    int black = 0, white = 0, total = BLANK_SAMPLE * BLANK_SAMPLE;
    for (int i = 0; i < total; i++) {
        if (grey[i] <= BLANK_BLACK) black++;
        else if (grey[i] >= BLANK_WHITE) white++;
    }
    av_free(grey);
    return black * 10 >= total * 8 || white * 10 >= total * 8;
}

/* Decodes the first frame at or after `seconds` into `frame`. Returns 1 on success. */
static int decode_frame_at(AVFormatContext *fmt_ctx, AVCodecContext *dec_ctx, int vsi,
                           int seconds, AVFrame *frame)
{
    AVStream *stream = fmt_ctx->streams[vsi];
    int64_t ts = av_rescale_q((int64_t)seconds * AV_TIME_BASE, AV_TIME_BASE_Q, stream->time_base);
    if (av_seek_frame(fmt_ctx, vsi, ts, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;
    avcodec_flush_buffers(dec_ctx);

    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return 0;
    int got = 0;
    /* counted in video packets, not packets: a file's audio would otherwise use up the budget
       before the video reached the time we are seeking to */
    int decoded = 0;
    while (! got && decoded < 900) {
        if (av_read_frame(fmt_ctx, pkt) < 0) break;
        if (pkt->stream_index == vsi) {
            decoded++;
            if (avcodec_send_packet(dec_ctx, pkt) >= 0 && avcodec_receive_frame(dec_ctx, frame) >= 0) {
                /* a seek lands on the keyframe before the time asked for, which on a video with
                   sparse keyframes can be a long way before it: decode on to the frame we came
                   for, or every candidate time comes back with the same early frame */
                int64_t shown = frame->best_effort_timestamp;
                if (shown != AV_NOPTS_VALUE && shown < ts)
                    av_frame_unref(frame);
                else
                    got = 1;
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return got;
}

/* Where to look for a frame worth showing. 1s in is where a thumbnail came from before
   any of this, and is still what most videos get; the rest are for the ones that open on
   a fade, a black screen or a title card. */
static const int FRAME_CANDIDATE_SECONDS[] = { 1, 5, 10, 15, 20, 25, 30 };

/* ── JNI entry points ─────────────────────────────────────────────────────── */

JNIEXPORT jbyteArray JNICALL
Java_org_peergos_thumbnailer_VideoThumbnailer_generate(
        JNIEnv *env, jclass cls, jstring jpath, jint maxSize)
{
    const char *path = (*env)->GetStringUTFChars(env, jpath, NULL);

    AVFormatContext *fmt_ctx        = NULL;
    AVCodecContext  *dec_ctx        = NULL;
    AVFrame         *src_frame      = NULL;
    AVFrame         *fallback_frame = NULL;
    jbyteArray       result         = NULL;

    if (avformat_open_input(&fmt_ctx, path, NULL, NULL) < 0) goto cleanup;
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) goto cleanup;

    int vsi = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vsi < 0) goto cleanup;
    AVStream *stream = fmt_ctx->streams[vsi];
    int rotation = get_video_rotation(stream);

    const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!dec) goto cleanup;
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) goto cleanup;
    if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0) goto cleanup;
    dec_ctx->thread_count = 1;
    if (avcodec_open2(dec_ctx, dec, NULL) < 0) goto cleanup;

    src_frame = av_frame_alloc();
    if (!src_frame) goto cleanup;

    /* A video that is blank wherever we look still gets a thumbnail: the first frame we
       decoded is no worse than the nothing it had before. */
    int64_t duration_secs = fmt_ctx->duration == AV_NOPTS_VALUE ? 0 : fmt_ctx->duration / AV_TIME_BASE;
    int candidates = sizeof(FRAME_CANDIDATE_SECONDS) / sizeof(FRAME_CANDIDATE_SECONDS[0]);
    int have_frame = 0;
    for (int i = 0; i < candidates; i++) {
        int at = FRAME_CANDIDATE_SECONDS[i];
        if (i > 0 && duration_secs > 0 && at >= duration_secs)
            break;
        av_frame_unref(src_frame);
        if (! decode_frame_at(fmt_ctx, dec_ctx, vsi, at, src_frame))
            break;
        if (! frame_is_blank(src_frame)) {
            have_frame = 1;
            break;
        }
        if (! fallback_frame)
            fallback_frame = av_frame_clone(src_frame);
    }
    if (! have_frame) {
        if (! fallback_frame) goto cleanup;
        av_frame_free(&src_frame);
        src_frame = fallback_frame;
        fallback_frame = NULL;
    }

    result = encode_webp_video(env, src_frame, (int)maxSize, rotation);

cleanup:
    if (fallback_frame) av_frame_free(&fallback_frame);
    if (src_frame) av_frame_free(&src_frame);
    if (dec_ctx)   avcodec_free_context(&dec_ctx);
    if (fmt_ctx)   avformat_close_input(&fmt_ctx);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return result;
}

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

    int orientation = get_exif_orientation(stream, src_frame);
    result = encode_webp_image(env, src_frame, (int)maxSize, orientation);

cleanup:
    if (pkt)       av_packet_free(&pkt);
    if (src_frame) av_frame_free(&src_frame);
    if (dec_ctx)   avcodec_free_context(&dec_ctx);
    if (fmt_ctx)   avformat_close_input(&fmt_ctx);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return result;
}
