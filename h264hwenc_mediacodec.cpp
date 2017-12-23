#define LOG_TAG "h264hwenc"

// ����ͷ�ļ�
#include <jni.h>
#include <stdlib.h>
#include <pthread.h>
// CLEMENS
//#include <utils/Log.h>
#include <android/log.h>

#include "ffencoder.h"
#include "h264hwenc.h"

// �ڲ����Ͷ���
// h264hwenc context
typedef struct {
    int         iw;
    int         ih;
    int         ow;
    int         oh;
    jmethodID   init;
    jmethodID   free;
    jmethodID   enqueue;
    jmethodID   dequeue;
    jobject     object;
    void       *ffencoder;
} H264ENC;

extern    JavaVM* g_jvm;
JNIEXPORT JNIEnv* get_jni_env(void);

// ����ʵ��
void *h264hwenc_mediacodec_init(int iw, int ih, int ow, int oh, int frate, int bitrate, void *ffencoder)
{
    JNIEnv  *env = get_jni_env();
    H264ENC *enc = (H264ENC*)calloc(1, sizeof(H264ENC));
    if (!enc) {
        // CLEMENS
        // CLEMENS
        // ALOGE("failed to allocate h264hwenc context !\n");
        __android_log_print(ANDROID_LOG_ERROR, "h264hwenc_mediacodec", "failed to allocate h264hwenc context !\n");        
        return NULL;
    }

    jclass    h264enc_class = (jclass)env->FindClass("com/apical/dvr/H264HwEncoder");
    jmethodID constructor   = (jmethodID)env->GetMethodID(h264enc_class, "<init>", "()V");
    enc->init      = (jmethodID)env->GetMethodID(h264enc_class, "init", "(IIII)V");
    enc->free      = (jmethodID)env->GetMethodID(h264enc_class, "free", "()V");
    enc->enqueue   = (jmethodID)env->GetMethodID(h264enc_class, "enqueueInputBuffer", "([BJI)Z");
    enc->dequeue   = (jmethodID)env->GetMethodID(h264enc_class, "dequeueOutputBuffer", "(I)[B");
    enc->iw         = iw;
    enc->ih         = ih;
    enc->ow         = ow;
    enc->oh         = oh;
    enc->ffencoder = ffencoder;

    // new H264HwEncoder
    jobject obj = env->NewObject(h264enc_class, constructor);

    // create global ref
    enc->object = env->NewGlobalRef(obj);

    // delete local ref
    env->DeleteLocalRef(h264enc_class);
    env->DeleteLocalRef(obj);

    // init
    env->CallVoidMethod(enc->object, enc->init, ow, oh, frate, bitrate);

    return enc;
}

void h264hwenc_mediacodec_close(void *ctxt)
{
    JNIEnv  *env = get_jni_env();
    H264ENC *enc = (H264ENC*)ctxt;
    if (!enc) return;

    // release
    env->CallVoidMethod(enc->object, enc->free);

    // delete global ref
    env->DeleteGlobalRef(enc->object);

    // free
    free(enc);
}

int h264hwenc_mediacodec_picture_format(void *ctxt)
{
    return AV_PIX_FMT_NV12;
}

int h264hwenc_mediacodec_picture_alloc(void *ctxt, AVFrame *frame)
{
    JNIEnv  *env = get_jni_env();
    H264ENC *enc = (H264ENC*)ctxt;
    if (!enc) return -1;

    jbyteArray array   = env->NewByteArray(enc->ow * enc->oh * 12 / 8);
    frame->width       = enc->ow;
    frame->height      = enc->oh;
    frame->format      = AV_PIX_FMT_NV12;
    frame->opaque      = (jbyteArray)env->NewGlobalRef(array);
    frame->data[0]     = (uint8_t*)env->GetByteArrayElements(array, 0);
    frame->data[1]     = frame->data[0] + enc->ow * enc->oh;
    frame->linesize[0] = enc->ow;
    frame->linesize[1] = enc->ow;
    env->DeleteLocalRef(array);
    return 0;
}

int h264hwenc_mediacodec_picture_free(void *ctxt, AVFrame *frame)
{
    JNIEnv  *env = get_jni_env();
    H264ENC *enc = (H264ENC*)ctxt;
    if (!enc) return -1;

    jbyteArray array = (jbyteArray)frame->opaque;
    env->ReleaseByteArrayElements(array, (jbyte*)frame->data[0], 0);
    env->DeleteGlobalRef(array);
    return 0;
}

int h264hwenc_mediacodec_encode(void *ctxt, AVFrame *frame, int timeout)
{
    JNIEnv  *env = get_jni_env();
    H264ENC *enc = (H264ENC*)ctxt;
    if (!enc) return -1;

    // enqueue picture data
    jboolean ret = env->CallBooleanMethod(enc->object, enc->enqueue,
                    (jbyteArray)frame->opaque, (jlong)frame->pts, timeout);
    if (!ret) return -1;

    // dequeue h264 data
    jbyteArray array = (jbyteArray)env->CallObjectMethod(enc->object, enc->dequeue, timeout);
    if (!array) return -1;

    uint8_t *buffer = (uint8_t*)env->GetByteArrayElements(array, 0);
    int      flags  = buffer[4] == 0x67 ? AV_PKT_FLAG_KEY : 0;
    ffencoder_write_video_frame(enc->ffencoder, flags, buffer, env->GetArrayLength(array), frame->pts);

    env->ReleaseByteArrayElements(array, (jbyte*)buffer, 0);
    env->DeleteLocalRef(array);

    return ret ? 0 : -1;
}

