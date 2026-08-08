/*
* Copyright (c) 2018-2023 NVIDIA Corporation
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without
* restriction, including without limitation the rights to use,
* copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the software, and to permit persons to whom the
* software is furnished to do so, subject to the following
* conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
* HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*/


#pragma once
#include <cstring>
#include <sstream>
#include <vector>
#include <map>
#include <assert.h>
#include <stdio.h>
#include <typeinfo>
#include <cctype>
#include <type_traits>
#include <algorithm>
#include <functional>

#define NVOF_ARGS_PARSE(parser, argc, argv)          \
    try {                                            \
        (parser).Parse((argc), (argv));              \
    } catch(const std::exception &e) {               \
        std::cerr << e.what() << std::endl;          \
         exit(1);                                    \
    }

template <typename T>
std::string ToString(T val)
{
    std::ostringstream stream;
    stream << val;
    return stream.str();
}

template <>
std::string ToString(bool val)
{
    std::ostringstream stream;
    stream << std::boolalpha << val;
    return stream.str();
}

template <>
std::string ToString(std::string val)
{
    std::ostringstream stream;
    stream << (val.empty() ? "None" : val);
    return stream.str();
}

bool ParseOption(const std::string& arg,
    const std::string& option,
    const std::function<bool(const std::string&)>& parseFunc,
    bool bRequiresValue,
    bool* bHasValue)
{
    bool hasSingleDash = arg.length() > 1 && arg[0] == '-';
    *bHasValue = false;
    if (hasSingleDash)
    {
        bool hasDoubleDash = arg.length() > 2 && arg[1] == '-';
        std::string key = arg.substr(hasDoubleDash ? 2 : 1);
        int val = 1;
        std::string value = std::to_string(val);
        size_t equalsPos = key.find('=');
        if (equalsPos != std::string::npos)
        {
            value = key.substr(equalsPos + 1);
            key = key.substr(0, equalsPos);
        }
        else if (bRequiresValue)
        {
            return false;
        }
        if (key != option)
        {
            return false;
        }
        *bHasValue = parseFunc(value);
        return true;
    }
    return false;
}

template <typename T>
bool ParseOption(const std::string& optValue, T& optVal)
{
    std::istringstream stream(optValue);
    stream >> optVal;
    if (!stream.eof() && !stream.good()) {
        return false;
    }
    return true;
}

template <>
bool ParseOption(const std::string& optValue, bool& optVal)
{
    std::string temp = optValue;
    std::transform(temp.begin(), temp.end(), temp.begin(),
        [](char c) -> char { return (char)(std::tolower(c)); });
    std::istringstream is(temp);
    is >> (temp.size() > 1 ? std::boolalpha : std::noboolalpha) >> optVal;
    return true;
}

template <typename T>
std::string GetTypeName(T val)
{
    if (std::is_same<T, int32_t>::value)
    {
        return "int32";
    }
    else if (std::is_same<T, int64_t>::value)
    {
        return "int64";
    }
    else if (std::is_same<T, float>::value)
    {
        return "float";
    }
    else if (std::is_same<T, std::string>::value)
    {
        return "string";
    }
    else if (std::is_same<T, bool>::value)
    {
        return "bool";
    }
    return "unknown";
}

/*
 * The NvOFOption class represents an option (or command-line parameter) which
 * can accept at most one value. If the option requires a value, the type of
 * that values can be one of int32_t, int64_t, float, std::string and bool.
 */
class NvOFOption
{
public:
    template <typename T>
    static NvOFOption CreateOption(const char* name, const char* usage, T& val, bool bRequiresValue = true)
    {
        auto callbackFunc = [&val](const std::string& optValue) ->bool
        {
            return ParseOption(optValue, val);
        };

        auto printFunc = [&val]() -> std::string
        {
            return ToString(val);
        };

        return NvOFOption(name, callbackFunc, usage,
            printFunc, GetTypeName(val), ToString(val), bRequiresValue);
    }
private:

    NvOFOption(const char* name,
        std::function<bool(const std::string&)> callback,
        const char* desc,
        std::function<std::string()> printFunc,
        const std::string& typeName,
        const std::string& defaultValue,
        bool bRequiresValue)
        : m_name(name),
        m_callback(callback),
        m_getOptValueForDisplay(printFunc),
        m_usageDesc(desc),
        m_typeName(typeName),
        m_defaultValueForDisplay(defaultValue),
        m_bRequiresValue(bRequiresValue)
    {

    }

    bool Parse(const std::string& arg, bool* bHasValue) const
    {
        return ParseOption(arg, m_name, m_callback, m_bRequiresValue, bHasValue);
    }

    std::string m_name;
    std::string m_defaultValueForDisplay;
    std::function<bool(const std::string&)> m_callback;
    std::function<std::string()> m_getOptValueForDisplay;

    std::string m_usageDesc;
    std::string m_typeName;
    bool m_bRequiresValue;
    friend class NvOFCmdParser;
};

/*
 * The NvOFCmdParser class represents a parser for command-line options.
 */
class NvOFCmdParser
{
public:

    NvOFCmdParser()
    {

    }
    template <typename T>
    void AddOptions(const char* szName, T& value, const char* szDesc)
    {
        m_options.emplace_back(NvOFOption::CreateOption(szName, szDesc, value));
        auto option = m_options.back();
        std::ostringstream oss;
        oss << "--" << option.m_name << "=<" << option.m_typeName << ">";
        m_maxArgWidth = m_maxArgWidth > oss.str().size() ? m_maxArgWidth : oss.str().size();
    }

    bool Parse(int argc, const char** argv)
    {
        bool result = true;
        if (argc == 1 || std::strcmp(argv[1], "--help") == 0)
        {
            std::cout << help(argv[0]) << std::endl;
            exit(0);
        }
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--help") != 0)
            {
                bool bFound = false;
                for (const auto &option : m_options)
                {
                    bool bHasValue;
                    bFound = option.Parse(argv[i], &bHasValue);
                    if (!bHasValue)
                    {
                        result = false;
                    }
                    if (bFound)
                    {
                        break;
                    }
                }
                if (!bFound)
                {
                    std::ostringstream oss;
                    oss << "Error parsing \"" << argv[i] << "\"" << std::endl;
                    oss << help(argv[0]);
                    throw std::invalid_argument(oss.str());
                }
            }
        }
        return result && (argc < 2 || std::strcmp(argv[1], "--help") != 0);
    }

    std::string help(const std::string& cmdline)
    {
        const uint32_t maxTypeNameSize = 10;
        std::string appName;
        size_t pos_s = cmdline.find_last_of("/\\");
        if (pos_s == std::string::npos)
        {
            appName = cmdline;
        }
        else
        {
            appName = cmdline.substr(pos_s + 1, cmdline.length() - pos_s);
        }
        std::stringstream ss;
        ss << "Usage: " << appName.c_str() << " [params] " << std::endl;
        for (const auto &option : m_options)
        {
            std::stringstream opt;
            auto token = std::string("--");
            opt << token << option.m_name << "=<" << option.m_typeName << ">";
            auto curWidth = opt.str().size();
            ss << opt.str() << std::string(m_maxArgWidth - curWidth, ' ') << "\t"
               << option.m_usageDesc << std::endl;
            ss << std::string(m_maxArgWidth, ' ') << "\t"
               << "Default value: " << option.m_defaultValueForDisplay << std::endl;
        }

        return ss.str();
    }
private:
    std::vector<NvOFOption> m_options;
    size_t m_maxArgWidth = 0;
};
