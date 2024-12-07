//
// Created by geier on 30/04/2020.
//

#ifndef RENDERINGX_NDKARRAYHELPER_H
#define RENDERINGX_NDKARRAYHELPER_H

#include <jni.h>
#include <cassert>
#include <string>
#include <type_traits>
#include <vector>

//
// The purpose of this namespace is to make it easier to transfer arrays of generic data types
// (Data types that are both used by cpp and java, like int, float ) via the NDK from java to cpp
// Only dependencies are standard libraries and the android java NDK
//
namespace NDKArrayHelper
{
// workaround from https://en.cppreference.com/w/cpp/language/if#Constexpr_If
// To have compile time type safety
template <typename T, typename T2>
struct always_false : std::false_type
{};

/**
 * Returns a std::vector whose size depends on the size of the java array
 * and which owns the underlying memory. Most generic and memory safe.
 * Function has compile time type safety, see example() below
 * @param TCpp basic cpp type like int, float.
 * @param TJava java array like jintArray, jfloatArray - has to match the cpp type. For example,
 * T==int => T2==jintArray
 * If TCpp==int and TJava==jintArray this method returns std::vector<int>
 */
template <class TCpp, class TJava>
static std::vector<TCpp> DynamicSizeArray(JNIEnv* env, TJava jArray)
{
    const size_t      size = (size_t) env->GetArrayLength(jArray);
    std::vector<TCpp> ret(size);
    if constexpr (std::is_same_v<TCpp, bool> && std::is_same_v<TJava, jbooleanArray>)
    {
        env->GetBooleanArrayRegion(jArray, 0, size, ret.data());
    }
    else if constexpr (std::is_same_v<TCpp, float> && std::is_same_v<TJava, jfloatArray>)
    {
        env->GetFloatArrayRegion(jArray, 0, size, ret.data());
    }
    else if constexpr (std::is_same_v<TCpp, int> && std::is_same_v<TJava, jintArray>)
    {
        env->GetIntArrayRegion(jArray, 0, size, ret.data());
    }
    else if constexpr (std::is_same_v<TCpp, double> && std::is_same_v<TJava, jdoubleArray>)
    {
        env->GetDoubleArrayRegion(jArray, 0, size, ret.data());
    }
    else
    {
        // a) Make sure you use the right combination.
        // For example, if you want a std::vector<float> pass a jfloatArray as second parameter
        // b) Make sure you use a supported type. (e.g. one that appears in the above if - else)
        static_assert(always_false<TCpp, TJava>::value, "Unsupported Combination / Type");
    }
    return ret;
}

/**
 *  Whenever size is already known at compile time u can use this one
 *  but note that it is impossible to check at compile time if the java array has the same size
 *  Assert at run time if size!=array size
 */
template <class T, std::size_t S, class T2>
static std::array<T, S> FixedSizeArray(JNIEnv* env, T2 array)
{
    const auto       data = DynamicSizeArray<T>(env, array);
    std::array<T, S> ret;
    assert(data.size() == S);
    std::memcpy(ret.data(), data.data(), data.size() * sizeof(T));
    return ret;
}

static std::string DynamicSizeString(JNIEnv* env, jstring jstring1)
{
    const char*       valueP = env->GetStringUTFChars(jstring1, nullptr);
    const std::string ret    = std::string(valueP);
    env->ReleaseStringUTFChars(jstring1, valueP);
    return ret;
}

// Demonstrate the type safety of DynamicSizeArray:
// X Compiles, but Y does not (which is exactly what we want)
static void example()
{
    JNIEnv*            env         = nullptr;
    jfloatArray        jFloatArray = nullptr;
    std::vector<float> X           = DynamicSizeArray<float>(env, jFloatArray);
    // This one does not compile - we cannot get a int array from a java float array
    // std::vector<int> Y=DynamicSizeArray<int>(env,jFloatArray);
}
}  // namespace NDKArrayHelper

#endif  // RENDERINGX_NDKARRAYHELPER_H
