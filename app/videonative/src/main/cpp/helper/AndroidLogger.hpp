//
// Created by Consti10 on 14/05/2019.
//

#ifndef FPV_VR_PRIVATE_MDEBUG_H
#define FPV_VR_PRIVATE_MDEBUG_H

#include <string.h>
#include <cassert>
#include <sstream>
#include "android/log.h"

// remove any old c style definitions that might have slip trough some header files
#ifdef LOGD
#undef LOGD
#endif
#ifdef LOGE
#undef LOGE
#endif

// See https://medium.com/@geierconstantinabc/best-way-to-log-in-android-native-code-c-style-7461005610f6
// Handles logging in all my android studio projects with native code
// inspired by
// https://android.googlesource.com/platform/system/core/+/refs/heads/master/base/include/android-base/logging.h Most
// notable difference : I have LOGD(TAG) they have LOG(SEVERITY)
class AndroidLogger
{
  public:
    // Chrome university https://www.youtube.com/watch?v=UNJrgsQXvCA
    // 'New style C++ ' https://google.github.io/styleguide/cppguide.html
    AndroidLogger(const android_LogPriority priority, const std::string TAG)
        : M_PRIORITY(priority), M_TAG(std::move(TAG))
    {
    }

    ~AndroidLogger() { logBigMessage(stream.str()); }

    AndroidLogger(const AndroidLogger& other) = delete;

  private:
    std::stringstream         stream;
    const std::string         M_TAG;
    const android_LogPriority M_PRIORITY;
    // taken from https://android.googlesource.com/platform/system/core/+/android-2.1_r1/liblog/logd_write.c
    static constexpr const auto ANDROID_LOG_BUFF_SIZE = 1024;

    // Splits debug messages that exceed the android log maximum length into smaller log(s)
    // Recursive declaration
    void logBigMessage(const std::string& message)
    {
        if (message.length() > ANDROID_LOG_BUFF_SIZE)
        {
            __android_log_print(M_PRIORITY, M_TAG.c_str(), "%s", message.substr(0, ANDROID_LOG_BUFF_SIZE).c_str());
            logBigMessage(message.substr(ANDROID_LOG_BUFF_SIZE));
        }
        else
        {
            __android_log_print(M_PRIORITY, M_TAG.c_str(), "%s", message.c_str());
        }
    }

    // the non-member function operator<< will now have access to private members
    template <typename T>
    friend AndroidLogger& operator<<(AndroidLogger& record, T&& t);
};

template <typename T>
AndroidLogger& operator<<(AndroidLogger& record, T&& t)
{
    record.stream << std::forward<T>(t);
    return record;
}

template <typename T>
AndroidLogger& operator<<(AndroidLogger&& record, T&& t)
{
    return record << std::forward<T>(t);
}

// taken from https://stackoverflow.com/questions/1666802/is-there-a-class-macro-in-c
// also see https://gcc.gnu.org/onlinedocs/gcc/Function-Names.html
namespace PrettyFunctionHelper
{
static constexpr const auto UNKNOWN_CLASS_NAME = "UnknownClassName";

/**
 * @param prettyFunction as obtained by the macro __PRETTY_FUNCTION__
 * @param function as obtained by the macro __FUNCTION__
 * @return a string containing the class name at the end, optionally prefixed by the namespace(s).
 * Example return values: "MyNamespace1::MyNamespace2::MyClassName","MyNamespace1::MyClassName" "MyClassName"
 */
static std::string namespaceAndClassName(const std::string& function, const std::string& prettyFunction)
{
    // AndroidLogger(ANDROID_LOG_DEBUG,"NoT")<<prettyFunction;
    //  Here I assume that the 'function name' does not appear multiple times. The opposite is highly unlikely
    const size_t len1 = prettyFunction.find(function);
    if (len1 == std::string::npos) return UNKNOWN_CLASS_NAME;
    // The substring of len-2 contains the function return type and the "namespaceAndClass" area
    const std::string returnTypeAndNamespaceAndClassName = prettyFunction.substr(0, len1 - 2);
    // find the last empty space in the substring. The values until the first empty space are the function return type
    // for example "void ","std::optional<std::string> ", "static std::string "
    // See how the 3rd example return type also contains a " ".
    // However, it is guaranteed that the area NamespaceAndClassName does not contain an empty space
    const size_t begin1 = returnTypeAndNamespaceAndClassName.rfind(" ");
    if (begin1 == std::string::npos) return UNKNOWN_CLASS_NAME;
    const std::string namespaceAndClassName = returnTypeAndNamespaceAndClassName.substr(begin1 + 1);
    return namespaceAndClassName;
}

/**
 * @param namespaceAndClassName value obtained by namespaceAndClassName()
 * @return the class name only (without namespace prefix if existing)
 */
static std::string className(const std::string& namespaceAndClassName)
{
    const size_t end = namespaceAndClassName.rfind("::");
    if (end != std::string::npos)
    {
        return namespaceAndClassName.substr(end + 2);
    }
    return namespaceAndClassName;
}

class Test
{
  public:
    static std::string testMacro(std::string unused)
    {
        const auto namespaceAndClassName =
            PrettyFunctionHelper::namespaceAndClassName(__FUNCTION__, __PRETTY_FUNCTION__);
        // AndroidLogger(ANDROID_LOG_DEBUG,"NoT2")<<namespaceAndClassName;
        assert(namespaceAndClassName.compare("PrettyFunctionHelper::Test") == 0);
        const auto className = PrettyFunctionHelper::className(namespaceAndClassName);
        // AndroidLogger(ANDROID_LOG_DEBUG,"NoT2")<<className;
        assert(className.compare("Test") == 0);
        return className;
    }
};

static const std::string x = Test::testMacro("");
namespace TestNamespace1
{
namespace TestNamespace2
{
class Test2
{
  public:
    static std::string testMacro()
    {
        const auto namespaceAndClassName =
            PrettyFunctionHelper::namespaceAndClassName(__FUNCTION__, __PRETTY_FUNCTION__);
        AndroidLogger(ANDROID_LOG_DEBUG, "NoT2") << namespaceAndClassName;
        assert(namespaceAndClassName.compare("PrettyFunctionHelper::TestNamespace1::TestNamespace2::Test2") == 0);
        const auto className = PrettyFunctionHelper::className(namespaceAndClassName);
        assert(className.compare("Test2") == 0);
        return className;
    }
};
}  // namespace TestNamespace2
}  // namespace TestNamespace1
// static const std::string x2=TestNamespace1::TestNamespace2::Test2::testMacro();
}  // namespace PrettyFunctionHelper
#ifndef ANDROID_LOGER_DEFINE_CUSTOM_CLASS_NAME_MACRO
#define __NAMESPACE_AND_CLASS_NAME__ PrettyFunctionHelper::namespaceAndClassName(__FUNCTION__, __PRETTY_FUNCTION__)
#define __CLASS_NAME__ PrettyFunctionHelper::className(__NAMESPACE_AND_CLASS_NAME__)
#endif  // ANDROID_LOGER_DEFINE_CUSTOM_CLASS_NAME_MACRO

// Here we use the current class name / namespace as tag (with pretty function workaround)
// Unfortunately we can only achieve that using a 'old c-style' macro
#define MLOGD AndroidLogger(ANDROID_LOG_DEBUG, __CLASS_NAME__)
#define MLOGE AndroidLogger(ANDROID_LOG_ERROR, __CLASS_NAME__)

// When using a custom TAG we do not need a macro. Use cpp style instead
static AndroidLogger MLOGD2(const std::string CUSTOM_TAG)
{
    return AndroidLogger(ANDROID_LOG_DEBUG, std::move(CUSTOM_TAG));
}

static AndroidLogger MLOGE2(const std::string CUSTOM_TAG)
{
    return AndroidLogger(ANDROID_LOG_ERROR, std::move(CUSTOM_TAG));
}

// print some example LOGs
namespace TEST_LOGGING_ON_ANDROID
{
static void test2()
{
    __android_log_print(ANDROID_LOG_DEBUG, "TAG", "Before");
    AndroidLogger(ANDROID_LOG_DEBUG, "MyTAG") << "Hello World I " << 1 << " F " << 0.0f << " X";
    __android_log_print(ANDROID_LOG_DEBUG, "TAG", "After");
}
}  // namespace TEST_LOGGING_ON_ANDROID

/*static void test(){
       LOG::D("TestText %d",1);
       LOG::D("TestText %d",1);
       const char*LOL="LOL";
       LOG::D("TestText %s",LOL);
       LOG::LOL("HALLO %s %d",LOL,2);
   }*/
// #define TAG_MDEBUG "MDebug"
// #define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG_MDEBUG, __VA_ARGS__)

/*namespace LOG{
    // taken from https://android.googlesource.com/platform/system/core/+/android-2.1_r1/liblog/logd_write.c
    static constexpr const auto ANDROID_LOG_BUFF_SIZE=1024;
    static constexpr auto DEFAULT_TAG="NoTag";
    static void D(const char* fmt,...)__attribute__((__format__(printf, 1, 2))) {
        va_list argptr;
        va_start(argptr, fmt);
        __android_log_vprint(ANDROID_LOG_DEBUG,DEFAULT_TAG,fmt,argptr);
        va_end(argptr);
    }
    static void E(const char* fmt,...)__attribute__((__format__(printf, 1, 2))) {
        va_list argptr;
        va_start(argptr, fmt);
        __android_log_vprint(ANDROID_LOG_ERROR,DEFAULT_TAG,fmt,argptr);
        va_end(argptr);
    }
    static void LOL(const char* fmt,...) {
        const auto ANDROID_LOG_BUFF_SIZE=1024;
        char buffer[ANDROID_LOG_BUFF_SIZE];
        va_list argptr;
        va_start(argptr, fmt);
        vsprintf (buffer,fmt, argptr);
        vsnprintf(buffer,ANDROID_LOG_BUFF_SIZE, fmt, argptr);
        va_end(argptr);
        __android_log_print(ANDROID_LOG_DEBUG,"HA","%s",buffer);
    }
}*/
#endif  // FPV_VR_PRIVATE_MDEBUG_H
