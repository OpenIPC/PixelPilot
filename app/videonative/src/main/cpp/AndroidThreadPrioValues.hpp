//
// Created by geier on 17/05/2020.
//

#ifndef FPVUE_ANDROIDTHREADPRIOVALUES_HPP
#define FPVUE_ANDROIDTHREADPRIOVALUES_HPP

// The values for android thread priorities and
// the values used by my FPV_VR app (2 different namespaces though)

namespace AndroidThreadPriorityValues
{
// This one was taken from https://android.googlesource.com/platform/system/core/+/jb-dev/include/system/graphics.h
constexpr auto HAL_PRIORITY_URGENT_DISPLAY = -8;
// values taken from
// https://android.googlesource.com/platform/frameworks/native/+/android-4.2.2_r1/include/utils/ThreadDefs.h
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        /*
         * ***********************************************
         * ** Keep in sync with android.os.Process.java **
         * ***********************************************
         *
         * This maps directly to the "nice" priorities we use in Android.
         * A thread priority should be chosen inverse-proportionally to
         * the amount of work the thread is expected to do. The more work
         * a thread will do, the less favorable priority it should get so that
         * it doesn't starve the system. Threads not behaving properly might
         * be "punished" by the kernel.
         * Use the levels below when appropriate. Intermediate values are
         * acceptable, preferably use the {MORE|LESS}_FAVORABLE constants below.
         */
        ANDROID_PRIORITY_LOWEST = 19,
        /* use for background tasks */
        ANDROID_PRIORITY_BACKGROUND = 10,

        /* most threads run at normal priority */
        ANDROID_PRIORITY_NORMAL = 0,

        /* threads currently running a UI that the user is interacting with */
        ANDROID_PRIORITY_FOREGROUND = -2,
        /* the main UI thread has a slightly more favorable priority */
        ANDROID_PRIORITY_DISPLAY = -4,

        /* ui service treads might want to run at a urgent display (uncommon) */
        ANDROID_PRIORITY_URGENT_DISPLAY = HAL_PRIORITY_URGENT_DISPLAY,

        /* all normal audio threads */
        ANDROID_PRIORITY_AUDIO = -16,

        /* service audio threads (uncommon) */
        ANDROID_PRIORITY_URGENT_AUDIO = -19,
        /* should never be used in practice. regular process might not
         * be allowed to use this level */
        ANDROID_PRIORITY_HIGHEST        = -20,
        ANDROID_PRIORITY_DEFAULT        = ANDROID_PRIORITY_NORMAL,
        ANDROID_PRIORITY_MORE_FAVORABLE = -1,
        ANDROID_PRIORITY_LESS_FAVORABLE = +1,
    };
#ifdef __cplusplus
}  // extern "C"
#endif
}  // namespace AndroidThreadPriorityValues

// All these values are for FPVue_Android
// namespace FPV_VR_PRIORITY{
//    constexpr int CPU_PRIORITY_GLRENDERER_STEREO_FB=-19; //This one needs a whole CPU core all the time anyways
//    constexpr int CPU_PRIORITY_GLRENDERER_STEREO=-16; //The GL thread also should get 1 whole cpu core
//    constexpr int CPU_PRIORITY_UDPRECEIVER_VIDEO=-16;  //needs low latency and does not use the cpu that much
//    constexpr int CPU_PRIORITY_DECODER_OUTPUT=-16;     //needs low latency and does not use the cpu that much
//    constexpr int CPU_PRIORITY_UVC_FRAME_CALLBACK=-17; //needs low latency but uses CPU a lot (decoding). More prio
//    than GLRenderer
//    // These are much lower
//    constexpr int CPU_PRIORITY_GLRENDERER_MONO=-4; //only shows the OSD not video
//    constexpr int CPU_PRIORITY_UDPRECEIVER_TELEMETRY=-4; //not as important as video but also needs almost no CPU
//    processing time constexpr int CPU_PRIORITY_UDPSENDER_HEADTRACKING=-4;
//}

#endif  // FPVUE_ANDROIDTHREADPRIOVALUES_HPP
