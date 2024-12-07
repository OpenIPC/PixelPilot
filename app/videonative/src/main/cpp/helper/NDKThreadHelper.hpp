//
// Created by geier on 12/05/2020.
//

#ifndef FPVUE_NDKTHREAD_H
#define FPVUE_NDKTHREAD_H

#include <jni.h>

//
// Workaround for issue https://github.com/android/ndk/issues/1255 and more
// Java Thread utility methods
// Another method that has proven really usefully is the JThread::isInterrupted() method
//
// Direct mapping from java Thread method names to c++
//
class JThread
{
  private:
    JNIEnv*   env;
    jclass    jcThread;
    jmethodID jmCurrentThread;
    jmethodID jmIsInterrupted;
    jmethodID jmGetPriority;
    jmethodID jmSetPriority;
    jobject   joCurrentThread;

  public:
    JThread(JNIEnv* env)
    {
        this->env       = env;
        jcThread        = env->FindClass("java/lang/Thread");
        jmCurrentThread = env->GetStaticMethodID(jcThread, "currentThread", "()Ljava/lang/Thread;");
        jmIsInterrupted = env->GetMethodID(jcThread, "isInterrupted", "()Z");
        jmGetPriority   = env->GetMethodID(jcThread, "getPriority", "()I");
        jmSetPriority   = env->GetMethodID(jcThread, "setPriority", "(I)V");
        joCurrentThread = env->CallStaticObjectMethod(jcThread, jmCurrentThread);
    }

    bool isInterrupted() { return (bool) env->CallBooleanMethod(joCurrentThread, jmIsInterrupted); }

    int getPriority() { return (int) env->CallIntMethod(joCurrentThread, jmGetPriority); }

    void setPriority(int wantedPriority) { env->CallVoidMethod(joCurrentThread, jmSetPriority, (jint) wantedPriority); }

    static bool isInterrupted(JNIEnv* env) { return JThread(env).isInterrupted(); }

    static int getPriority(JNIEnv* env) { return JThread(env).getPriority(); }

    static void setPriority(JNIEnv* env, int wantedPriority) { JThread(env).setPriority(wantedPriority); }

    static void printThreadPriority(JNIEnv* env)
    {
        // MLOGD << "printThreadPriority " << getPriority(env);
    }
};
// Java android.os.Process utility methods (not java/lang/Process !)
// I think process and thread is used in the same context here but you probably want to use
// Process.setPriority instead
namespace JProcess
{
// calls android.os.Process.setThreadPriority()
static void setThreadPriority(JNIEnv* env, int wantedPriority)
{
    jclass    jcProcess           = env->FindClass("android/os/Process");
    jmethodID jmSetThreadPriority = env->GetStaticMethodID(jcProcess, "setThreadPriority", "(I)V");
    env->CallStaticVoidMethod(jcProcess, jmSetThreadPriority, (jint) wantedPriority);
}

// calls android.os.Process.getThreadPriority(android.os.Process.myTid())
static int getThreadPriority(JNIEnv* env)
{
    jclass    jcProcess           = env->FindClass("android/os/Process");
    jmethodID jmGetThreadPriority = env->GetStaticMethodID(jcProcess, "getThreadPriority", "(I)I");
    jmethodID jmMyTid             = env->GetStaticMethodID(jcProcess, "myTid", "()I");
    jint      myTid               = env->CallStaticIntMethod(jcProcess, jmMyTid);
    return (int) env->CallStaticIntMethod(jcProcess, jmGetThreadPriority, (jint) myTid);
}

static void printThreadPriority(JNIEnv* env)
{
    // MLOGD<<"printThreadPriority "<<getThreadPriority(env);
}
}  // namespace JProcess

// Attach / detach JavaVM to thread
namespace NDKThreadHelper
{
static constexpr auto MY_DEFAULT_TAG = "NDKThreadHelper";

static JNIEnv* attachThread(JavaVM* jvm)
{
    JNIEnv*          myNewEnv;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;  // choose your JNI version
    args.name    = "Thread name";    // you might want to give the java thread a name
    args.group   = NULL;             // you might want to assign the java thread to a ThreadGroup
    jvm->AttachCurrentThread(&myNewEnv, &args);
    return myNewEnv;
}

static void detachThread(JavaVM* jvm)
{
    jvm->DetachCurrentThread();
}

// Thread needs to be bound to Java VM
static void setProcessThreadPriority(JNIEnv* env, int wantedPriority, const char* TAG = MY_DEFAULT_TAG)
{
    const int currentPriority = JProcess::getThreadPriority(env);
    JProcess::setThreadPriority(env, wantedPriority);
    const int newPriority = JProcess::getThreadPriority(env);
    //        if(newPriority==wantedPriority){
    //            MLOGD2(TAG)<<"Successfully set priority from "<<currentPriority<<" to"<<wantedPriority;
    //        }else{
    //            MLOGD2(TAG)<<"Cannot set priority from "<<currentPriority<<" to "<<wantedPriority<<" is
    //            "<<newPriority<<" instead";
    //        }
}

// If the current thread is already bound to the Java VM only call JProcess::setThreadPriority
// If no Java VM is attached (e.g. the thread was created via ndk std::thread or equivalent)
// attach the java vm, set priority and then DETACH again
static void setProcessThreadPriorityAttachDetach(JavaVM* vm, int wantedPriority, const char* TAG = MY_DEFAULT_TAG)
{
    JNIEnv* env = nullptr;
    vm->GetEnv((void**) &env, JNI_VERSION_1_6);
    bool detachWhenDone = false;
    if (env == nullptr)
    {
        // MLOGD2(TAG)<<"Attaching thread";
        env            = attachThread(vm);
        detachWhenDone = true;
    }
    setProcessThreadPriority(env, wantedPriority, TAG);
    if (detachWhenDone)
    {
        detachThread(vm);
    }
}

// set name of thread. print error on failure
static void setName(pthread_t pthread, const char* name)
{
    auto res = pthread_setname_np(pthread, name);
    if (res != 0)
    {
        // MLOGE<<"Cannot set thread name "<<std::string(name)<<"§ error"<<res;
    }
}
}  // namespace NDKThreadHelper

#include <thread>

namespace NDKTHREADHELPERTEST
{
// attach java VM, set Thread priority & print it.
// Then detach, reattach JVM and print thread priority
static void doSomething(JavaVM* jvm)
{
    JNIEnv* env = NDKThreadHelper::attachThread(jvm);
    JThread::printThreadPriority(env);
    JThread::setPriority(env, 5);
    JThread::printThreadPriority(env);
    NDKThreadHelper::detachThread(jvm);
    env = NDKThreadHelper::attachThread(jvm);
    JThread::printThreadPriority(env);
    NDKThreadHelper::detachThread(jvm);
}

// same as doSomething() but uses ProcessThreadPriority
static void doSomething2(JavaVM* jvm, int prio)
{
    JNIEnv* env = NDKThreadHelper::attachThread(jvm);
    JProcess::printThreadPriority(env);
    JProcess::setThreadPriority(env, prio);
    JProcess::printThreadPriority(env);
    NDKThreadHelper::detachThread(jvm);
    env = NDKThreadHelper::attachThread(jvm);
    JProcess::printThreadPriority(env);
    NDKThreadHelper::detachThread(jvm);
}

static void test(JNIEnv* env)
{
    JavaVM* jvm;
    env->GetJavaVM(&jvm);
    std::thread* thread1 = new std::thread(doSomething2, jvm, -20);
}
}  // namespace NDKTHREADHELPERTEST

#endif  // FPVUE_NDKTHREAD_H
