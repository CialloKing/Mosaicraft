#pragma once

#include <string>

namespace mosaicraft
{
inline constexpr auto featureSchemaProbe = "SELECT image_id,grid_lab,tiny,lbp FROM image_features LIMIT 0";

// 只探测已落盘的结构，不能把 immutable 连接用于读取可变数据。
inline std::string immutableLibraryUri(const std::string &genericPath)
{
    std::string uri = "file:";
#ifdef _WIN32
    uri += genericPath.rfind("//", 0) == 0 ? "//" : "/";
#endif
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char ch : genericPath)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == ':')
        {
            uri += static_cast<char>(ch);
        }
        else
        {
            uri += '%';
            uri += hex[ch >> 4];
            uri += hex[ch & 15];
        }
    }
    return uri + "?immutable=1";
}
}
