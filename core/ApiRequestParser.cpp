#include "ApiRequestParser.h"

#include "ApiMetadata.h"
#include "json.hpp"
#include "RequestValidation.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <climits>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <string>

namespace mosaicraft
{

namespace
{

using json = nlohmann::json;



bool validateFields(const json &body, ApiOperation operation, RequestError &error)
{
    for (const auto &endpoint : apiEndpointMetadata())
    {
        if (endpoint.operation != operation)
        {
            continue;
        }
        for (auto value = body.begin(); value != body.end(); ++value)
        {
            if (std::find(endpoint.requestFields.begin(), endpoint.requestFields.end(), value.key()) == endpoint.requestFields.end() || value->is_null())
            {
                error.field = value.key();
                error = "unknown or null field: " + value.key();
                return false;
            }
        }
        if (operation == ApiOperation::Mosaic || operation == ApiOperation::SubmitBuildJob)
        {
            for (const auto &field : endpoint.requiredFields)
            {
                if (!body.contains(field))
                {
                    error.field = field;
                    error = "required field: " + field;
                    return false;
                }
            }
        }
        return true;
    }
    error = "unsupported operation";
    return false;
}

bool parseStrictInt(const std::string &text, int &out)
{
    if (text.empty())
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    long value = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' || value < INT_MIN || value > INT_MAX)
    {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool parsePositiveInt(const std::string &text, int &out)
{
    int value = 0;
    if (!parseStrictInt(text, value) || value <= 0)
    {
        return false;
    }
    out = value;
    return true;
}

bool requireIntAtLeast(const std::string &field, int value, int minimum, RequestError &error)
{
    if (value >= minimum)
    {
        return true;
    }
    error.field = field;
    error = field + " must be at least " + std::to_string(minimum);
    return false;
}

bool requireIntBetween(const std::string &field, int value, int minimum, int maximum, RequestError &error)
{
    if (value >= minimum && value <= maximum)
    {
        return true;
    }
    error.field = field;
    error = field + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum);
    return false;
}

bool requireDoubleAtLeast(const std::string &field, double value, double minimum, RequestError &error)
{
    if (value >= minimum)
    {
        return true;
    }
    error.field = field;
    error = field + " must be at least " + std::to_string(minimum);
    return false;
}

bool requireDoubleBetween(const std::string &field, double value, double minimum, double maximum, RequestError &error)
{
    if (value >= minimum && value <= maximum)
    {
        return true;
    }
    error.field = field;
    error = field + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum);
    return false;
}

bool parseSize(const std::string &text, int &w, int &h)
{
    size_t sep = text.find_first_of("xX");
    if (sep == std::string::npos || sep == 0 || sep + 1 >= text.size())
    {
        return false;
    }
    if (text.find_first_of("xX", sep + 1) != std::string::npos)
    {
        return false;
    }
    int parsedW = 0;
    int parsedH = 0;
    if (!parsePositiveInt(text.substr(0, sep), parsedW) || !parsePositiveInt(text.substr(sep + 1), parsedH))
    {
        return false;
    }
    w = parsedW;
    h = parsedH;
    return true;
}

bool parseJsonObject(const std::string &body, json &values, RequestError &error)
{
    if (body.empty())
    {
        values = json::object();
        return true;
    }
    try
    {
        std::vector<std::set<std::string>> keys;
        auto uniqueKeys = [&](int, json::parse_event_t event, json &value)
        {
            if (event == json::parse_event_t::object_start)
            {
                keys.emplace_back();
            }
            else if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
            {
                throw std::runtime_error("duplicate JSON key: " + value.get<std::string>());
            }
            else if (event == json::parse_event_t::object_end)
            {
                keys.pop_back();
            }
            return true;
        };
        values = json::parse(body, uniqueKeys);
    }
    catch (const json::parse_error &e)
    {
        error = std::string("invalid JSON body: ") + e.what();
        return false;
    }
    catch (const std::runtime_error &e)
    {
        error = e.what();
        return false;
    }
    if (!values.is_object())
    {
        error = "JSON body must be an object";
        return false;
    }
    return true;
}

bool validateQuery(const ApiQueryParams &query, const std::string &body, ApiOperation operation, RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }
    for (const auto &endpoint : apiEndpointMetadata())
    {
        if (endpoint.operation != operation)
        {
            continue;
        }
        for (const auto &item : query)
        {
            if (std::find(endpoint.acceptedQueryKeys.begin(), endpoint.acceptedQueryKeys.end(), item.first) == endpoint.acceptedQueryKeys.end() || values.contains(item.first))
            {
                error.field = item.first;
                error = "unknown or repeated parameter: " + item.first;
                return false;
            }
        }
        return true;
    }
    return false;
}

bool getStringField(const json &body, const std::vector<std::string> &keys, std::string &out, RequestError &error)
{
    if (!error.empty())
    {
        return false;
    }
    for (const auto &key : keys)
    {
        auto it = body.find(key);
        if (it == body.end() || it->is_null())
        {
            continue;
        }
        if (!it->is_string())
        {
            error.field = keys.front();
            error = key + " must be a string";
            return false;
        }
        out = it->get<std::string>();
        return true;
    }
    return false;
}

bool getIntField(const json &body, const std::vector<std::string> &keys, int &out, RequestError &error)
{
    if (!error.empty())
    {
        return false;
    }
    for (const auto &key : keys)
    {
        auto it = body.find(key);
        if (it == body.end() || it->is_null())
        {
            continue;
        }
        if (!it->is_number_integer() && !it->is_number_unsigned())
        {
            error.field = keys.front();
            error = key + " must be a number";
            return false;
        }
        double value = it->get<double>();
        if (!std::isfinite(value) || value < static_cast<double>(std::numeric_limits<int>::min()) ||
            value > static_cast<double>(std::numeric_limits<int>::max()))
        {
            error.field = keys.front();
            error = key + " is out of range";
            return false;
        }
        if (std::floor(value) != value)
        {
            error.field = keys.front();
            error = key + " must be an integer";
            return false;
        }
        out = static_cast<int>(value);
        return true;
    }
    return false;
}

bool getDoubleField(const json &body, const std::vector<std::string> &keys, double &out, RequestError &error)
{
    if (!error.empty())
    {
        return false;
    }
    for (const auto &key : keys)
    {
        auto it = body.find(key);
        if (it == body.end() || it->is_null())
        {
            continue;
        }
        if (!it->is_number())
        {
            error.field = keys.front();
            error = key + " must be a number";
            return false;
        }
        double value = it->get<double>();
        if (!std::isfinite(value))
        {
            error.field = keys.front();
            error = key + " is out of range";
            return false;
        }
        out = value;
        return true;
    }
    return false;
}

bool getBoolField(const json &body, const std::vector<std::string> &keys, bool &out, RequestError &error)
{
    if (!error.empty())
    {
        return false;
    }
    for (const auto &key : keys)
    {
        auto it = body.find(key);
        if (it == body.end() || it->is_null())
        {
            continue;
        }
        if (!it->is_boolean())
        {
            error.field = keys.front();
            error = key + " must be a boolean";
            return false;
        }
        out = it->get<bool>();
        return true;
    }
    return false;
}

bool hasQuery(const ApiQueryParams &query, const std::string &key)
{
    return query.find(key) != query.end();
}

std::string getQuery(const ApiQueryParams &query, const std::string &key)
{
    auto it = query.find(key);
    return it == query.end() ? std::string() : it->second;
}

std::vector<std::string> keysFor(const std::string &field)
{
    return {field};
}

bool getQueryField(const ApiQueryParams &query, const std::string &field, std::string &out)
{
    for (const auto &key : keysFor(field))
    {
        if (hasQuery(query, key))
        {
            out = getQuery(query, key);
            return true;
        }
    }
    return false;
}

bool getQueryBool(const ApiQueryParams &query, const std::string &field,
                  bool defaultForPresence, bool &out, RequestError &error)
{
    std::string value;
    if (!getQueryField(query, field, value))
    {
        return false;
    }
    if (value == "true")
    {
        out = true;
        return true;
    }
    if (value == "false")
    {
        out = false;
        return true;
    }
    error.field = field;
    error = field + " must be a boolean";
    return false;
}

bool getQueryInt(const ApiQueryParams &query, const std::string &field, int &out,
                 RequestError &error)
{
    std::string value;
    if (!getQueryField(query, field, value))
    {
        return false;
    }
    if (!parseStrictInt(value, out))
    {
        error.field = field;
        error = field + " must be a number";
        return false;
    }
    return true;
}

} // namespace

bool parseMosaicRequestJson(const std::string &body, MosaicRequest &request, RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }

    std::string text;
    int intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;
    if (!validateFields(values, ApiOperation::Mosaic, error))
    {
        return false;
    }

    if (getStringField(values, keysFor("inputPath"), text, error))
    {
        request.inputPath = text;
    }
    if (getStringField(values, keysFor("dbPath"), text, error))
    {
        request.dbPath = text;
    }
    if (getStringField(values, keysFor("outputPath"), text, error))
    {
        request.outputPath = text;
    }

    auto &cfg = request.config;
    if (getIntField(values, keysFor("tileW"), intValue, error))
    {
        if (!requireIntAtLeast("tileW", intValue, 4, error))
        {
            return false;
        }
        cfg.tileW = intValue;
    }
    if (getIntField(values, keysFor("tileH"), intValue, error))
    {
        if (!requireIntAtLeast("tileH", intValue, 4, error))
        {
            return false;
        }
        cfg.tileH = intValue;
    }
    if (getIntField(values, keysFor("outW"), intValue, error))
    {
        if (!requireIntAtLeast("outW", intValue, 0, error))
        {
            return false;
        }
        cfg.outW = intValue;
    }
    if (getIntField(values, keysFor("outH"), intValue, error))
    {
        if (!requireIntAtLeast("outH", intValue, 0, error))
        {
            return false;
        }
        cfg.outH = intValue;
    }
    if (getIntField(values, keysFor("nativeTileW"), intValue, error))
    {
        if (!requireIntAtLeast("nativeTileW", intValue, 1, error))
        {
            return false;
        }
        cfg.nativeTileW = intValue;
    }
    if (getIntField(values, keysFor("nativeTileH"), intValue, error))
    {
        if (!requireIntAtLeast("nativeTileH", intValue, 1, error))
        {
            return false;
        }
        cfg.nativeTileH = intValue;
    }
    if (getIntField(values, keysFor("candidates"), intValue, error))
    {
        if (!requireIntAtLeast("candidates", intValue, 10, error))
        {
            return false;
        }
        cfg.candidates = intValue;
    }
    if (getIntField(values, keysFor("topNRandom"), intValue, error))
    {
        if (!requireIntAtLeast("topNRandom", intValue, 1, error))
        {
            return false;
        }
        cfg.topNRandom = intValue;
    }
    if (getIntField(values, keysFor("neighborWindow"), intValue, error))
    {
        if (!requireIntAtLeast("neighborWindow", intValue, 0, error))
        {
            return false;
        }
        cfg.neighborWindow = intValue;
    }
    if (getIntField(values, keysFor("upscale"), intValue, error))
    {
        if (!requireIntAtLeast("upscale", intValue, 1, error))
        {
            return false;
        }
        cfg.upscale = intValue;
    }
    if (getIntField(values, keysFor("quality"), intValue, error))
    {
        if (!requireIntBetween("quality", intValue, 1, 100, error))
        {
            return false;
        }
        cfg.jpegQuality = intValue;
    }
    if (getIntField(values, keysFor("pngLevel"), intValue, error))
    {
        if (!requireIntBetween("pngLevel", intValue, 1, 9, error))
        {
            return false;
        }
        cfg.pngCompressionLevel = intValue;
    }

    if (getDoubleField(values, keysFor("lRange"), doubleValue, error))
    {
        if (!requireDoubleAtLeast("lRange", doubleValue, 0.0, error))
        {
            return false;
        }
        cfg.lRange = doubleValue;
    }
    if (getDoubleField(values, keysFor("usePenalty"), doubleValue, error))
    {
        cfg.usePenalty = doubleValue;
    }
    if (getDoubleField(values, keysFor("labWeight"), doubleValue, error))
    {
        cfg.labWeight = doubleValue;
    }
    if (getDoubleField(values, keysFor("gridWeight"), doubleValue, error))
    {
        cfg.gridWeight = doubleValue;
    }
    if (getDoubleField(values, keysFor("tinyWeight"), doubleValue, error))
    {
        cfg.tinyWeight = doubleValue;
    }
    if (getDoubleField(values, keysFor("edgeWeight"), doubleValue, error))
    {
        cfg.edgeWeight = doubleValue;
    }
    if (getDoubleField(values, keysFor("lbpWeight"), doubleValue, error))
    {
        cfg.lbpWeight = doubleValue;
    }
    if (getDoubleField(values, keysFor("neighborPenalty"), doubleValue, error))
    {
        if (!requireDoubleAtLeast("neighborPenalty", doubleValue, 0.0, error))
        {
            return false;
        }
        cfg.neighborPenalty = doubleValue;
    }
    if (getDoubleField(values, keysFor("colorStrength"), doubleValue, error))
    {
        if (!requireDoubleBetween("colorStrength", doubleValue, 0.0, 0.5, error))
        {
            return false;
        }
        cfg.colorStrength = doubleValue;
    }

    if (getDoubleField(values, keysFor("overlayStrength"), doubleValue, error))
    {
        if (!requireDoubleBetween("overlayStrength", doubleValue, 0.0, 1.0, error))
        {
            return false;
        }
        cfg.overlayStrength = doubleValue;
    }

    if (getStringField(values, keysFor("format"), text, error))
    {
        if (!text.empty())
        {
            cfg.outputFormat = text;
            cfg.formatExplicit = true;
        }
    }
    if (getStringField(values, keysFor("writeMode"), text, error))
    {
        if (text == "auto" || text == "stream" || text == "batch")
        {
            cfg.writeMode = text;
        }
        else
        {
            error.field = "writeMode";
            error = "writeMode must be auto, stream, or batch";
            return false;
        }
    }


    if (getBoolField(values, keysFor("generatePreview"), boolValue, error))
    {
        cfg.generatePreview = boolValue;
    }
    if (getBoolField(values, keysFor("useGpu"), boolValue, error))
    {
        cfg.useGpu = boolValue;
    }

    if (getBoolField(values, keysFor("tiled"), boolValue, error))
    {
        cfg.tiledOutput = boolValue;
    }
    if (getBoolField(values, keysFor("deepZoom"), boolValue, error))
    {
        cfg.deepZoom = boolValue;
        if (boolValue)
        {
            cfg.tiledOutput = true;
        }
    }
    if (getBoolField(values, keysFor("colorAdjust"), boolValue, error))
    {
        cfg.colorAdjust = boolValue;
    }
    if (getBoolField(values, keysFor("adaptiveWeights"), boolValue, error))
    {
        cfg.adaptiveWeights = boolValue;
    }
    if (getBoolField(values, keysFor("analyze"), boolValue, error))
    {
        cfg.analyze = boolValue;
    }
    if (getBoolField(values, keysFor("benchmark"), boolValue, error))
    {
        cfg.benchmark = boolValue;
    }
    return error.empty() && validateMosaicRequest(request, error.message, &error.field);
}

bool parseBuildRequestJson(const std::string &body, BuildRequest &request, RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }

    std::string text;
    int intValue = 0;
    bool boolValue = false;
    if (!validateFields(values, ApiOperation::SubmitBuildJob, error))
    {
        return false;
    }

    if (getStringField(values, keysFor("inputDir"), text, error))
    {
        request.inputDir = text;
    }
    if (getStringField(values, keysFor("outputDir"), text, error))
    {
        request.outputDir = text;
    }
    if (getStringField(values, keysFor("dbPath"), text, error))
    {
        request.dbPath = text;
    }

    if (getIntField(values, keysFor("threads"), intValue, error))
    {
        if (!requireIntAtLeast("threads", intValue, 0, error))
        {
            return false;
        }
        request.threads = intValue;
    }
    if (getIntField(values, keysFor("normalizeWidth"), intValue, error))
    {
        if (!requireIntAtLeast("normalizeWidth", intValue, 1, error))
        {
            return false;
        }
        request.normalizeWidth = intValue;
    }
    if (getIntField(values, keysFor("normalizeHeight"), intValue, error))
    {
        if (!requireIntAtLeast("normalizeHeight", intValue, 1, error))
        {
            return false;
        }
        request.normalizeHeight = intValue;
    }

    if (getBoolField(values, keysFor("appendMode"), boolValue, error))
    {
        request.appendMode = boolValue;
    }
    if (getBoolField(values, keysFor("normalizeOnly"), boolValue, error))
    {
        request.normalizeOnly = boolValue;
    }
    if (getBoolField(values, keysFor("forceMode"), boolValue, error))
    {
        request.forceMode = boolValue;
    }
    if (getBoolField(values, keysFor("recursive"), boolValue, error))
    {
        request.recursive = boolValue;
    }

    request.allowPrompt = false;
    return error.empty() && validateBuildRequest(request, error.message, &error.field);
}

bool applyDatabaseRequestJson(const std::string &body, DatabaseRequest &request, RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }

    std::string text;
    if (!validateFields(values, ApiOperation::DatabaseStats, error))
    {
        return false;
    }
    if (getStringField(values, keysFor("dbPath"), text, error))
    {
        request.dbPath = text;
    }
    return error.empty();
}

bool applyDatabaseUsageRequestJson(const std::string &body, DatabaseUsageRequest &request, RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }

    std::string text;
    int intValue = 0;
    bool boolValue = false;
    if (!validateFields(values, ApiOperation::DatabaseUsage, error))
    {
        return false;
    }
    if (getStringField(values, keysFor("dbPath"), text, error))
    {
        request.dbPath = text;
    }
    if (getIntField(values, keysFor("limit"), intValue, error))
    {
        if (!requireIntAtLeast("limit", intValue, 1, error))
        {
            return false;
        }
        request.limit = intValue;
    }
    if (getBoolField(values, keysFor("showUnused"), boolValue, error))
    {
        request.showUnused = boolValue;
    }
    return error.empty();
}

bool applyDatabaseUsageExportRequestJson(const std::string &body, DatabaseUsageExportRequest &request,
                                         RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }

    std::string text;
    bool boolValue = false;
    if (!validateFields(values, ApiOperation::DatabaseUsageExport, error))
    {
        return false;
    }
    if (getStringField(values, keysFor("dbPath"), text, error))
    {
        request.dbPath = text;
    }
    if (getStringField(values, keysFor("outputDir"), text, error))
    {
        request.outputDir = text;
    }
    if (getBoolField(values, keysFor("confirm"), boolValue, error))
    {
        request.confirm = boolValue;
    }
    return error.empty();
}

bool applyDatabasePurgeRequestJson(const std::string &body, DatabasePurgeRequest &request, RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }

    std::string text;
    bool boolValue = false;
    if (!validateFields(values, ApiOperation::DatabasePurge, error))
    {
        return false;
    }
    if (getStringField(values, keysFor("dbPath"), text, error))
    {
        request.dbPath = text;
    }
    if (getBoolField(values, keysFor("dryRun"), boolValue, error))
    {
        request.dryRun = boolValue;
    }
    if (getBoolField(values, keysFor("confirm"), boolValue, error))
    {
        request.confirm = boolValue;
    }
    return error.empty();
}

bool applyInspectRequestJson(const std::string &body, InspectRequest &request, RequestError &error)
{
    json values;
    if (!parseJsonObject(body, values, error))
    {
        return false;
    }

    std::string text;
    if (!validateFields(values, ApiOperation::Inspect, error))
    {
        return false;
    }
    if (getStringField(values, keysFor("imagePath"), text, error))
    {
        request.imagePath = text;
    }
    if (getStringField(values, keysFor("dbPath"), text, error))
    {
        request.dbPath = text;
    }
    return error.empty();
}

bool parseDatabaseRequestApi(const ApiQueryParams &query, const std::string &body, DatabaseRequest &request,
                             RequestError &error)
{
    if (!validateQuery(query, body, ApiOperation::DatabaseStats, error))
    {
        return false;
    }
    std::string text;
    if (getQueryField(query, "dbPath", text))
    {
        request.dbPath = text;
    }
    return applyDatabaseRequestJson(body, request, error);
}

bool parseDatabaseUsageRequestApi(const ApiQueryParams &query, const std::string &body, DatabaseUsageRequest &request,
                                  RequestError &error)
{
    if (!validateQuery(query, body, ApiOperation::DatabaseUsage, error))
    {
        return false;
    }
    std::string text;
    int intValue = 0;
    bool boolValue = false;
    if (getQueryField(query, "dbPath", text))
    {
        request.dbPath = text;
    }
    if (getQueryInt(query, "limit", intValue, error))
    {
        if (!requireIntAtLeast("limit", intValue, 1, error))
        {
            return false;
        }
        request.limit = intValue;
    }
    if (!error.empty())
    {
        return false;
    }
    if (getQueryBool(query, "showUnused", false, boolValue, error))
    {
        request.showUnused = boolValue;
    }
    if (!error.empty())
    {
        return false;
    }
    return applyDatabaseUsageRequestJson(body, request, error);
}

bool parseDatabaseUsageExportRequestApi(const ApiQueryParams &query, const std::string &body,
                                        DatabaseUsageExportRequest &request, RequestError &error)
{
    if (!validateQuery(query, body, ApiOperation::DatabaseUsageExport, error))
    {
        return false;
    }
    std::string text;
    bool boolValue = false;
    if (getQueryField(query, "dbPath", text))
    {
        request.dbPath = text;
    }
    if (getQueryField(query, "outputDir", text))
    {
        request.outputDir = text;
    }
    if (getQueryBool(query, "confirm", false, boolValue, error))
    {
        request.confirm = boolValue;
    }
    if (!error.empty())
    {
        return false;
    }
    return applyDatabaseUsageExportRequestJson(body, request, error);
}

bool parseDatabasePurgeRequestApi(const ApiQueryParams &query, const std::string &body, DatabasePurgeRequest &request,
                                  RequestError &error)
{
    if (!validateQuery(query, body, ApiOperation::DatabasePurge, error))
    {
        return false;
    }
    std::string text;
    bool boolValue = false;
    if (getQueryField(query, "dbPath", text))
    {
        request.dbPath = text;
    }
    if (getQueryBool(query, "dryRun", true, boolValue, error))
    {
        request.dryRun = boolValue;
    }
    if (!error.empty())
    {
        return false;
    }
    if (getQueryBool(query, "confirm", false, boolValue, error))
    {
        request.confirm = boolValue;
    }
    if (!error.empty())
    {
        return false;
    }
    return applyDatabasePurgeRequestJson(body, request, error);
}

bool parseInspectRequestApi(const ApiQueryParams &query, const std::string &body, InspectRequest &request,
                            RequestError &error)
{
    if (!validateQuery(query, body, ApiOperation::Inspect, error))
    {
        return false;
    }
    std::string text;
    if (getQueryField(query, "imagePath", text))
    {
        request.imagePath = text;
    }
    if (getQueryField(query, "dbPath", text))
    {
        request.dbPath = text;
    }
    return applyInspectRequestJson(body, request, error);
}

bool parseMosaicRequestJson(const std::string &body, MosaicRequest &request, std::string &error)
{
    RequestError detail;
    const bool ok = parseMosaicRequestJson(body, request, detail);
    error = detail.message;
    return ok;
}

bool parseBuildRequestJson(const std::string &body, BuildRequest &request, std::string &error)
{
    RequestError detail;
    const bool ok = parseBuildRequestJson(body, request, detail);
    error = detail.message;
    return ok;
}

bool applyDatabaseRequestJson(const std::string &body, DatabaseRequest &request, std::string &error)
{
    RequestError detail;
    const bool ok = applyDatabaseRequestJson(body, request, detail);
    error = detail.message;
    return ok;
}

bool applyDatabaseUsageRequestJson(const std::string &body, DatabaseUsageRequest &request, std::string &error)
{
    RequestError detail;
    const bool ok = applyDatabaseUsageRequestJson(body, request, detail);
    error = detail.message;
    return ok;
}

bool applyDatabaseUsageExportRequestJson(const std::string &body, DatabaseUsageExportRequest &request,
                                         std::string &error)
{
    RequestError detail;
    const bool ok = applyDatabaseUsageExportRequestJson(body, request, detail);
    error = detail.message;
    return ok;
}

bool applyDatabasePurgeRequestJson(const std::string &body, DatabasePurgeRequest &request, std::string &error)
{
    RequestError detail;
    const bool ok = applyDatabasePurgeRequestJson(body, request, detail);
    error = detail.message;
    return ok;
}

bool applyInspectRequestJson(const std::string &body, InspectRequest &request, std::string &error)
{
    RequestError detail;
    const bool ok = applyInspectRequestJson(body, request, detail);
    error = detail.message;
    return ok;
}

bool parseDatabaseRequestApi(const ApiQueryParams &query, const std::string &body, DatabaseRequest &request,
                             std::string &error)
{
    RequestError detail;
    const bool ok = parseDatabaseRequestApi(query, body, request, detail);
    error = detail.message;
    return ok;
}

bool parseDatabaseUsageRequestApi(const ApiQueryParams &query, const std::string &body, DatabaseUsageRequest &request,
                                  std::string &error)
{
    RequestError detail;
    const bool ok = parseDatabaseUsageRequestApi(query, body, request, detail);
    error = detail.message;
    return ok;
}

bool parseDatabaseUsageExportRequestApi(const ApiQueryParams &query, const std::string &body,
                                        DatabaseUsageExportRequest &request, std::string &error)
{
    RequestError detail;
    const bool ok = parseDatabaseUsageExportRequestApi(query, body, request, detail);
    error = detail.message;
    return ok;
}

bool parseDatabasePurgeRequestApi(const ApiQueryParams &query, const std::string &body, DatabasePurgeRequest &request,
                                  std::string &error)
{
    RequestError detail;
    const bool ok = parseDatabasePurgeRequestApi(query, body, request, detail);
    error = detail.message;
    return ok;
}

bool parseInspectRequestApi(const ApiQueryParams &query, const std::string &body, InspectRequest &request,
                            std::string &error)
{
    RequestError detail;
    const bool ok = parseInspectRequestApi(query, body, request, detail);
    error = detail.message;
    return ok;
}

} // namespace mosaicraft
