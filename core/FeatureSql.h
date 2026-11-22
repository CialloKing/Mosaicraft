#pragma once

#include "FeatureIO.h"
#include "FeaturePayload.h"
#include <algorithm>
#include <sqlite3.h>

namespace mosaicraft
{
inline const uint8_t *featureBlob(sqlite3_stmt *statement, int column, size_t bytes)
{
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB ||
        sqlite3_column_bytes(statement, column) != static_cast<int>(bytes))
    {
        return nullptr;
    }
    return static_cast<const uint8_t *>(sqlite3_column_blob(statement, column));
}

inline bool decodeFeatureBlob(sqlite3_stmt *statement, int column, float *values, size_t count)
{
    const auto *blob = featureBlob(statement, column, count * sizeof(float));
    if (!blob)
    {
        return false;
    }
    decodeFloats(blob, values, count);
    return std::all_of(values, values + count, [](float value)
    {
        return std::isfinite(value);
    });
}

inline bool decodeTextures(sqlite3_stmt *statement, int column, TextureFeatures &texture)
{
    const auto *tiny = featureBlob(statement, column, texture.tiny.size());
    if (!tiny)
    {
        return false;
    }
    std::copy_n(tiny, texture.tiny.size(), texture.tiny.begin());
    texture.tinyValid = true;
    texture.lbpValid = decodeFeatureBlob(statement, column + 1, texture.lbp.data(), texture.lbp.size());
    return texture.lbpValid;
}
}
