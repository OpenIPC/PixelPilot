//
// Created by Constantin on 09.10.2017.
//

#ifndef OSDTESTER_STRINGHELPER_H
#define OSDTESTER_STRINGHELPER_H

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include "AndroidLogger.hpp"

// Various helper functions to create / modify strings
class StringHelper
{
  private:
    // Return the n of digits without the sign
    static const size_t countDigitsWithoutSign(unsigned long n) { return std::floor(std::log10(n) + 1); }

    // Return n of digits with sign (e.g. the '-' also counts as a digit)
    static const size_t countDigitsWithSign(long n)
    {
        if (n == 0) return 1;
        if (n < 0) return countDigitsWithoutSign(std::abs(n)) + 1;
        return countDigitsWithoutSign(n);
    }

    // Return n of digits with sign, slower than the one above
    // Only use for testing
    static const size_t countDigitsWithSignSlow(long n) { return std::to_string(n).length(); }

  public:
    // convert std::wstring to std::string
    static const std::string normalS(std::wstring& input) { return std::string(input.begin(), input.end()); }

    /**
     * If the value fits into a string of length @param maxLength return the value as string
     * Else return 'E' string
     */
    static const std::wstring intToWString(const int value, const size_t maxLength)
    {
        assert(maxLength >= 1);
        const auto asString = std::to_wstring(value);
        if (asString.length() > maxLength)
        {
            return L"E";
        }
        return asString;
    }

    /**
     * Returns the fractional and non-fractional parts of value as a w-string
     * @param value The value to write
     * @param maxLength  Maximum length of the string,including '-' and '.'
     * @param wantedPrecisionAfterCome The wanted precision after the come, if possible
     */
    static const std::wstring doubleToWString(double value, int maxLength, int wantedPrecisionAfterCome)
    {
        assert(maxLength >= 1);
        // 'whole number' is the part before the '.'
        const auto digitsWholeNumberWithSign = countDigitsWithSign((int) value);
        // Return error when not even the whole number fits into maxLength
        if (digitsWholeNumberWithSign > maxLength)
        {
            return L"E";
        }
        // Return the whole number when only the whole number fits (and if only the whole number and the '.'
        // fits, also return the whole number)
        if (digitsWholeNumberWithSign >= (maxLength - 1))
        {
            return std::to_wstring((int) value);
        }
        const std::wstring valueAsStringWithDecimals =
            (std::wstringstream() << std::fixed << std::setprecision(wantedPrecisionAfterCome) << value).str();
        return valueAsStringWithDecimals.substr(0, maxLength);
    }

    static const void doubleToString(
        std::wstring& sBeforeCome, std::wstring& sAfterCome, double value, int maxLength, int maxResAfterCome)
    {
        const auto valueAsString = doubleToWString(value, maxLength, maxResAfterCome);
        const auto idx           = valueAsString.find(L".");
        const auto nonFractional = idx == std::wstring::npos ? valueAsString : valueAsString.substr(0, idx);
        const auto fractional    = idx == std::wstring::npos ? L"" : valueAsString.substr(idx, valueAsString.length());
        sBeforeCome              = nonFractional;
        sAfterCome               = fractional;
    }

    /**
     * Convert a std::vector into a nice readable representation.
     * Example: input std::vector<int>{0,1,2} -> output [0,1,2]
     **/
    template <typename T>
    static std::string vectorAsString(const std::vector<T>& v)
    {
        std::stringstream ss;
        ss << "[";
        int count = 0;
        for (const auto i : v)
        {
            if constexpr (std::is_same_v<T, uint8_t>)
            {
                ss << (int) i;
            }
            else
            {
                ss << i;
            }
            count++;
            if (count != v.size())
            {
                ss << ",";
            }
        }
        ss << "]";
        return ss.str();
    }

    /**
     * If @param sizeBytes exceeds 1 mB / 1 kB use mB / kB as unit
     **/
    static std::string memorySizeReadable(const size_t sizeBytes)
    {
        // more than one MB
        if (sizeBytes > 1024 * 1024)
        {
            float sizeMB = (float) sizeBytes / 1024.0 / 1024.0;
            return std::to_string(sizeMB) + "mB";
        }
        // more than one KB
        if (sizeBytes > 1024)
        {
            float sizeKB = (float) sizeBytes / 1024.0;
            return std::to_string(sizeKB) + "kB";
        }
        return std::to_string(sizeBytes) + "B";
    }

  public:
    // Some simple testing
    static void testCountDigits()
    {
        std::srand(std::time(nullptr));
        MLOGD << "testCountDigits() start";
        std::vector<int> values = {-100, 0, 1, 9, 10, 100, 100};
        for (int i = 0; i < 5000; i++)
        {
            values.push_back(std::rand());
        }
        for (int value : values)
        {
            const size_t size1 = countDigitsWithSign(value);
            const size_t size2 = countDigitsWithSignSlow(value);
            MLOGD << "Value:" << value << " " << size1 << " " << size2;
            assert(size1 == size2);
        }
        MLOGD << "testCountDigits() end";
    }

    static void testIntToWString()
    {
        auto tmp = intToWString(100, 3);
        assert(tmp.compare(L"100") == 0);
        tmp = intToWString(1000, 3);
        assert(tmp.compare(L"E") == 0);
        tmp = intToWString(-100, 4);
        assert(tmp.compare(L"-100") == 0);
        tmp = intToWString(-1000, 4);
        assert(tmp.compare(L"E") == 0);
    }

    static void testDoubleToWString()
    {
        //  MLOGD<<StringHelper::normalS(tmp);
        auto tmp = doubleToWString(100, 6, 2);
        assert(tmp.compare(L"100.00") == 0);
        tmp = doubleToWString(100, 3, 2);
        assert(tmp.compare(L"100") == 0);
        tmp = doubleToWString(100.01, 6, 2);
        assert(tmp.compare(L"100.01") == 0);
        tmp = doubleToWString(100.01, 6, 3);
        assert(tmp.compare(L"100.01") == 0);
        tmp = doubleToWString(100.01, 7, 3);
        assert(tmp.compare(L"100.010") == 0);
    }

    static void test1()
    {
        doubleToWString(10.9234, 10, 4);
        doubleToWString(10.9234, 10, 2);
        doubleToWString(10.9234, 10, 0);
        doubleToWString(10.9234, 4, 4);
        doubleToWString(-10.9234, 4, 4);
    }
};

#endif  // OSDTESTER_STRINGHELPER_H
