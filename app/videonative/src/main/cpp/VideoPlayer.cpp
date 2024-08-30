#include <jni.h>
#include "VideoPlayer.h"
#include "AndroidThreadPrioValues.hpp"
#include "helper/NDKThreadHelper.hpp"
#include "helper/NDKHelper.hpp"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <fstream>

#define TAG "pixelpilot"

VideoPlayer::VideoPlayer(JNIEnv *env, jobject context) :
        mParser{std::bind(&VideoPlayer::onNewNALU, this, std::placeholders::_1)},
        videoDecoder(env) {
    env->GetJavaVM(&javaVm);
    videoDecoder.registerOnDecoderRatioChangedCallback([this](const VideoRatio ratio) {
        const bool changed = ratio != this->latestVideoRatio;
        this->latestVideoRatio = ratio;
        latestVideoRatioChanged = changed;
    });
    videoDecoder.registerOnDecodingInfoChangedCallback([this](const DecodingInfo info) {
        const bool changed = info != this->latestDecodingInfo;
        this->latestDecodingInfo = info;
        latestDecodingInfoChanged = changed;
    });
}

static int write_callback(int64_t offset, const void *buffer, size_t size, void *token) {
    FILE *f = (FILE *) token;
    fseek(f, offset, SEEK_SET);
    return fwrite(buffer, 1, size, f) != size;
}

void VideoPlayer::processQueue() {
    ::FILE *fout = fdopen(dvr_fd, "wb");
    MP4E_mux_t *mux = MP4E_open(0 /*sequential_mode*/, dvr_mp4_fragmentation, fout, write_callback);
    mp4_h26x_writer_t mp4wr;
    float framerate = 0;

    while (true) {
        last_dvr_write = get_time_ms();
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait(lock, [this] { return !naluQueue.empty() || stopFlag; });
        if (stopFlag) {
            break;
        }
        if (!naluQueue.empty()) {
            NALU nalu = naluQueue.front();
            if (framerate == 0) {
                if (latestDecodingInfo.currentFPS <= 0) {
                    continue;
                }
                if (MP4E_STATUS_OK != mp4_h26x_write_init(&mp4wr, mux, latestVideoRatio.width,
                                                          latestVideoRatio.height,
                                                          nalu.IS_H265_PACKET)) {
                    __android_log_print(ANDROID_LOG_DEBUG, TAG,
                                        "error: mp4_h26x_write_init failed");
                }
                framerate = latestDecodingInfo.currentFPS;
                __android_log_print(ANDROID_LOG_DEBUG, TAG,
                                    "mp4 init with fps=%.2f, res=%dx%d, hevc=%d", framerate,
                                    latestVideoRatio.width, latestVideoRatio.height,
                                    nalu.IS_H265_PACKET);
            }
            naluQueue.pop();
            lock.unlock();
            // Process the NALU
            auto res = mp4_h26x_write_nal(&mp4wr, nalu.getData(), nalu.getSize(),
                                          90000 / framerate);
            if (MP4E_STATUS_OK != res) {
                __android_log_print(ANDROID_LOG_DEBUG, TAG,
                                    "mp4_h26x_write_nal failed with %d", res);
            }
        }
    }

    MP4E_close(mux);
    mp4_h26x_write_close(&mp4wr);
    if (fout) {
        fclose(fout);
        fout = NULL;
    }
    dvr_fd = -1;
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "dvr thread done");
}

//Not yet parsed bit stream (e.g. raw h264 or rtp data)
void VideoPlayer::onNewVideoData(const uint8_t *data, const std::size_t data_length) {
    //MLOGD << "onNewVideoData " << data_length;
    mParser.parse_rtp_stream(data, data_length);
}

void VideoPlayer::onNewNALU(const NALU &nalu) {
    videoDecoder.interpretNALU(nalu);
    if (dvr_fd <= 0 || latestDecodingInfo.currentFPS <= 0) {
        return;
    }
    // Copy data to write if from a different thread.
    uint8_t *m_data_copy = new uint8_t[nalu.getSize()];
    memcpy(m_data_copy, nalu.getData(), nalu.getSize());
    NALU nalu_(m_data_copy, nalu.getSize(), nalu.IS_H265_PACKET);
    enqueueNALU(nalu_);
}

void VideoPlayer::setVideoSurface(JNIEnv *env, jobject surface) {
    //reset the parser so the statistics start again from 0
    // mParser.reset();
    //set the jni object for settings
    videoDecoder.setOutputSurface(env, surface);
}


void VideoPlayer::start(JNIEnv *env, jobject androidContext) {
    AAssetManager *assetManager = NDKHelper::getAssetManagerFromContext2(env, androidContext);
    //mParser.setLimitFPS(-1); //Default: Real time !
    const int VS_PORT = 5600;
    mUDPReceiver.release();
    mUDPReceiver = std::make_unique<UDPReceiver>(javaVm, VS_PORT, "UdpReceiver", -16,
                                                 [this](const uint8_t *data,
                                                        size_t data_length) {
                                                     onNewVideoData(data, data_length);
                                                 }, WANTED_UDP_RCVBUF_SIZE);
    mUDPReceiver->startReceiving();
}

void VideoPlayer::stop(JNIEnv *env, jobject androidContext) {
    if (mUDPReceiver) {
        mUDPReceiver->stopReceiving();
        mUDPReceiver.reset();
    }
}

std::string VideoPlayer::getInfoString() const {
    std::stringstream ss;
    if (mUDPReceiver) {
        ss << "Listening for video on port " << mUDPReceiver->getPort();
        ss << "\nReceived: " << mUDPReceiver->getNReceivedBytes() << "B"
           << " | parsed frames: ";
        // << mParser.nParsedNALUs << " | key frames: " << mParser.nParsedKonfigurationFrames;
    } else {
        ss << "Not receiving udp raw / rtp / rtsp";
    }
    return ss.str();
}


void VideoPlayer::startDvr(JNIEnv *env, jint fd, jint dvr_fmp4_enabled) {
    dvr_fd = dup(fd);
    dvr_mp4_fragmentation = dvr_fmp4_enabled;
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "dvr_fd=%d", dvr_fd);
    if (dvr_fd == -1) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG,
                            "Failed to duplicate dvr file descriptor");
        return;
    }
    startProcessing();
}

void VideoPlayer::stopDvr() {
    stopProcessing();
}

//----------------------------------------------------JAVA bindings---------------------------------------------------------------
#define JNI_METHOD(return_type, method_name) \
  JNIEXPORT return_type JNICALL              \
      Java_com_openipc_videonative_VideoPlayer_##method_name

inline jlong jptr(VideoPlayer *videoPlayerN) {
    return reinterpret_cast<intptr_t>(videoPlayerN);
}

inline VideoPlayer *native(jlong ptr) {
    return reinterpret_cast<VideoPlayer *>(ptr);
}


extern "C" {


extern "C"
JNIEXPORT jlong JNICALL
Java_com_openipc_videonative_VideoPlayer_nativeInitialize(JNIEnv *env, jclass clazz,
                                                          jobject context) {
    auto *p = new VideoPlayer(env, context);
    return jptr(p);
}

JNI_METHOD(void, nativeFinalize)
(JNIEnv *env, jclass jclass1, jlong videoPlayerN) {
    VideoPlayer *p = native(videoPlayerN);
    delete (p);
}

JNI_METHOD(void, nativeStart)
(JNIEnv *env, jclass jclass1, jlong videoPlayerN, jobject androidContext) {
    native(videoPlayerN)->start(env, androidContext);
}

JNI_METHOD(void, nativeStop)
(JNIEnv *env, jclass jclass1, jlong videoPlayerN, jobject androidContext) {
    native(videoPlayerN)->stop(env, androidContext);
}

JNI_METHOD(void, nativeSetVideoSurface)
(JNIEnv *env, jclass jclass1, jlong videoPlayerN, jobject surface) {
    native(videoPlayerN)->setVideoSurface(env, surface);
}

JNI_METHOD(jstring, getVideoInfoString)
(JNIEnv *env, jclass jclass1, jlong testReceiverN) {
    VideoPlayer *p = native(testReceiverN);
    jstring ret = env->NewStringUTF(p->getInfoString().c_str());
    return ret;
}

JNI_METHOD(jboolean, anyVideoDataReceived)
(JNIEnv *env, jclass jclass1, jlong testReceiverN) {
    VideoPlayer *p = native(testReceiverN);
    if (p->mUDPReceiver == nullptr) {
        return (jboolean) false;
    }
    bool ret = (p->mUDPReceiver->getNReceivedBytes() > 0);
    return (jboolean) ret;
}

JNI_METHOD(jboolean, receivingVideoButCannotParse)
(JNIEnv *env, jclass jclass1, jlong testReceiverN) {
    VideoPlayer *p = native(testReceiverN);
//    if(p->mUDPReceiver){
//        return (jboolean) (p->mUDPReceiver->getNReceivedBytes() > 1024 * 1024 && p->mParser.nParsedNALUs == 0);
//    }
    return (jboolean) false;
}

JNI_METHOD(jboolean, anyVideoBytesParsedSinceLastCall)
(JNIEnv *env, jclass jclass1, jlong testReceiverN) {
    VideoPlayer *p = native(testReceiverN);
    long nalusSinceLast = 0;// p->mParser.nParsedNALUs - p->nNALUsAtLastCall;
    p->nNALUsAtLastCall += nalusSinceLast;
    return (jboolean) (nalusSinceLast > 0);
}


JNI_METHOD(void, nativeCallBack)
(JNIEnv *env, jclass jclass1, jobject videoParamsChangedI, jlong testReceiverN) {
    VideoPlayer *p = native(testReceiverN);
    //Update all java stuff
    if (p->latestDecodingInfoChanged || p->latestVideoRatioChanged) {
        jclass jClassExtendsIVideoParamsChanged = env->GetObjectClass(videoParamsChangedI);
        if (p->latestVideoRatioChanged) {
            jmethodID onVideoRatioChangedJAVA = env->GetMethodID(jClassExtendsIVideoParamsChanged,
                                                                 "onVideoRatioChanged", "(II)V");
            env->CallVoidMethod(videoParamsChangedI, onVideoRatioChangedJAVA,
                                (jint) p->latestVideoRatio.width,
                                (jint) p->latestVideoRatio.height);
            p->latestVideoRatioChanged = false;
        }
        if (p->latestDecodingInfoChanged) {
            jclass jcDecodingInfo = env->FindClass("com/openipc/videonative/DecodingInfo");
            assert(jcDecodingInfo != nullptr);
            jmethodID jcDecodingInfoConstructor = env->GetMethodID(jcDecodingInfo, "<init>",
                                                                   "(FFFFFIIII)V");
            assert(jcDecodingInfoConstructor != nullptr);
            const auto info = p->latestDecodingInfo;
            auto decodingInfo = env->NewObject(jcDecodingInfo, jcDecodingInfoConstructor,
                                               (jfloat) info.currentFPS,
                                               (jfloat) info.currentKiloBitsPerSecond,
                                               (jfloat) info.avgParsingTime_ms,
                                               (jfloat) info.avgWaitForInputBTime_ms,
                                               (jfloat) info.avgDecodingTime_ms, (jint) info.nNALU,
                                               (jint) info.nNALUSFeeded,
                                               (jint) info.nDecodedFrames,
                                               (jint) info.nCodec);
            assert(decodingInfo != nullptr);
            jmethodID onDecodingInfoChangedJAVA = env->GetMethodID(jClassExtendsIVideoParamsChanged,
                                                                   "onDecodingInfoChanged",
                                                                   "(Lcom/openipc/videonative/DecodingInfo;)V");
            assert(onDecodingInfoChangedJAVA != nullptr);
            env->CallVoidMethod(videoParamsChangedI, onDecodingInfoChangedJAVA, decodingInfo);
            p->latestDecodingInfoChanged = false;
        }
    }
}


}

extern "C"
JNIEXPORT void JNICALL
Java_com_openipc_videonative_VideoPlayer_nativeStartDvr(JNIEnv *env, jclass clazz,
                                                        jlong native_instance,
                                                        jint fd, jint fmp4_enabled) {
    native(native_instance)->startDvr(env, fd, fmp4_enabled);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_openipc_videonative_VideoPlayer_nativeStopDvr(JNIEnv *env, jclass clazz,
                                                       jlong native_instance) {
    native(native_instance)->stopDvr();
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_openipc_videonative_VideoPlayer_nativeIsRecording(JNIEnv *env, jclass clazz,
                                                           jlong native_instance) {
    return native(native_instance)->isRecording();
}
