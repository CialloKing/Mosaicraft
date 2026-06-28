#include "ApiRequestParser.h"

#include "json.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>

namespace mosaicraft
{

namespace
{

using json = nlohmann::json;

bool parseSize(const std::string& text, int& w, int& h)
{
    size_t sep = text.find('x');
    if (sep == std::string::npos) sep = text.find('X');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= text.size()) return false;
    w = std::max(1, std::atoi(text.substr(0, sep).c_str()));
    h = std::max(1, std::atoi(text.substr(sep + 1).c_str()));
    return true;
}

bool parseJsonObject(const std::string& body, json& values, std::string& error)
{
    if (body.empty()) {
        values = json::object();
        return true;
    }
    try {
        values = json::parse(body);
    } catch (const json::parse_error& e) {
        error = std::string("invalid JSON body: ") + e.what();
        return false;
    }
    if (!values.is_object()) {
        error = "JSON body must be an object";
        return false;
    }
    return true;
}

bool getStringField(const json& body,
                    std::initializer_list<const char*> keys,
                    std::string& out,
                    std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_string()) {
            error = std::string(key) + " must be a string";
            return false;
        }
        out = it->get<std::string>();
        return true;
    }
    return false;
}

bool getIntField(const json& body,
                 std::initializer_list<const char*> keys,
                 int& out,
                 std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_number()) {
            error = std::string(key) + " must be a number";
            return false;
        }
        out = static_cast<int>(it->get<double>());
        return true;
    }
    return false;
}

bool getDoubleField(const json& body,
                    std::initializer_list<const char*> keys,
                    double& out,
                    std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_number()) {
            error = std::string(key) + " must be a number";
            return false;
        }
        out = it->get<double>();
        return true;
    }
    return false;
}

bool getBoolField(const json& body,
                  std::initializer_list<const char*> keys,
                  bool& out,
                  std::string& error)
{
    if (!error.empty()) return false;
    for (const char* key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_boolean()) {
            error = std::string(key) + " must be a boolean";
            return false;
        }
        out = it->get<bool>();
        return true;
    }
    return false;
}

} // namespace

bool parseMosaicRequestJson(const std::string& body,
                            MosaicRequest& request,
                            std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    int intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;

    if (getStringField(values, {"inputPath", "input"}, text, error))
        request.inputPath = text;
    if (getStringField(values, {"dbPath", "db"}, text, error))
        request.dbPath = text;
    if (getStringField(values, {"outputPath", "output"}, text, error))
        request.outputPath = text;

    auto& cfg = request.config;
    if (getIntField(values, {"tileW"}, intValue, error)) cfg.tileW = std::max(4, intValue);
    if (getIntField(values, {"tileH"}, intValue, error)) cfg.tileH = std::max(4, intValue);
    if (getIntField(values, {"outW"}, intValue, error)) cfg.outW = intValue > 0 ? intValue : 0;
    if (getIntField(values, {"outH"}, intValue, error)) cfg.outH = intValue > 0 ? intValue : 0;
    if (getIntField(values, {"nativeTileW"}, intValue, error)) cfg.nativeTileW = std::max(1, intValue);
    if (getIntField(values, {"nativeTileH"}, intValue, error)) cfg.nativeTileH = std::max(1, intValue);
    if (getIntField(values, {"candidates"}, intValue, error)) cfg.candidates = std::max(10, intValue);
    if (getIntField(values, {"topNrandom", "topNRandom"}, intValue, error))
        cfg.topNrandom = std::max(1, intValue);
    if (getIntField(values, {"neighborWindow"}, intValue, error)) cfg.neighborWindow = intValue;
    if (getIntField(values, {"upscale"}, intValue, error)) cfg.upscale = std::max(1, intValue);
    if (getIntField(values, {"quality", "jpegQuality"}, intValue, error))
        cfg.jpegQuality = std::max(1, std::min(100, intValue));
    if (getIntField(values, {"pngLevel", "pngCompressionLevel"}, intValue, error))
        cfg.pngCompressionLevel = std::max(1, std::min(9, intValue));

    if (getDoubleField(values, {"lRange"}, doubleValue, error)) cfg.lRange = doubleValue;
    if (getDoubleField(values, {"usePenalty", "penalty"}, doubleValue, error))
        cfg.usePenalty = doubleValue;
    if (getDoubleField(values, {"labWeight"}, doubleValue, error)) cfg.labWeight = doubleValue;
    if (getDoubleField(values, {"gridWeight"}, doubleValue, error)) cfg.gridWeight = doubleValue;
    if (getDoubleField(values, {"tinyWeight"}, doubleValue, error)) cfg.tinyWeight = doubleValue;
    if (getDoubleField(values, {"edgeWeight"}, doubleValue, error)) cfg.edgeWeight = doubleValue;
    if (getDoubleField(values, {"lbpWeight"}, doubleValue, error)) cfg.lbpWeight = doubleValue;
    if (getDoubleField(values, {"neighborPenalty"}, doubleValue, error)) cfg.neighborPenalty = doubleValue;
    if (getDoubleField(values, {"colorStrength"}, doubleValue, error))
        cfg.colorStrength = std::max(0.0, std::min(0.5, doubleValue));

    if (getStringField(values, {"format", "outputFormat"}, text, error)) {
        if (!text.empty()) {
            cfg.outputFormat = text;
            cfg.formatExplicit = true;
        }
    }
    if (getStringField(values, {"writeMode"}, text, error)) {
        if (text == "auto" || text == "stream" || text == "batch") {
            cfg.writeMode = text;
        } else {
            error = "writeMode must be auto, stream, or batch";
            return false;
        }
    }
    if (getStringField(values, {"outputTile"}, text, error)) {
        if (!parseSize(text, cfg.nativeTileW, cfg.nativeTileH)) {
            error = "outputTile must use WxH format";
            return false;
        }
    }

    if (getBoolField(values, {"useGpu"}, boolValue, error)) cfg.useGpu = boolValue;
    if (getBoolField(values, {"cpu"}, boolValue, error) && boolValue) cfg.useGpu = false;
    if (getBoolField(values, {"tiled", "tiledOutput"}, boolValue, error))
        cfg.tiledOutput = boolValue;
    if (getBoolField(values, {"deepZoom", "deepzoom"}, boolValue, error)) {
        cfg.deepZoom = boolValue;
        if (boolValue) cfg.tiledOutput = true;
    }
    if (getBoolField(values, {"colorAdjust"}, boolValue, error)) cfg.colorAdjust = boolValue;
    if (getBoolField(values, {"adaptiveWeights"}, boolValue, error)) cfg.adaptiveWeights = boolValue;
    if (getBoolField(values, {"analyze"}, boolValue, error)) cfg.analyze = boolValue;
    if (getBoolField(values, {"benchmark"}, boolValue, error)) cfg.benchmark = boolValue;

    return error.empty();
}

bool parseBuildRequestJson(const std::string& body,
                           BuildRequest& request,
                           std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    int intValue = 0;
    bool boolValue = false;

    if (getStringField(values, {"inputDir", "input"}, text, error))
        request.inputDir = text;
    if (getStringField(values, {"outputDir", "output"}, text, error))
        request.outputDir = text;
    if (getStringField(values, {"dbPath", "db"}, text, error))
        request.dbPath = text;
    if (getStringField(values, {"normalizeSize"}, text, error)) {
        if (!parseSize(text, request.normalizeWidth, request.normalizeHeight)) {
            error = "normalizeSize must use WxH format";
            return false;
        }
    }
    if (getIntField(values, {"threads"}, intValue, error))
        request.threads = std::max(0, intValue);
    if (getIntField(values, {"normalizeWidth"}, intValue, error))
        request.normalizeWidth = std::max(1, intValue);
    if (getIntField(values, {"normalizeHeight"}, intValue, error))
        request.normalizeHeight = std::max(1, intValue);

    if (getBoolField(values, {"appendMode", "append"}, boolValue, error))
        request.appendMode = boolValue;
    if (getBoolField(values, {"normalizeOnly"}, boolValue, error))
        request.normalizeOnly = boolValue;
    if (getBoolField(values, {"forceMode", "force"}, boolValue, error))
        request.forceMode = boolValue;
    if (getBoolField(values, {"recursive"}, boolValue, error))
        request.recursive = boolValue;

    request.allowPrompt = false;
    return error.empty();
}

bool applyDatabaseRequestJson(const std::string& body,
                              DatabaseRequest& request,
                              std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    if (getStringField(values, {"dbPath", "db"}, text, error)) request.dbPath = text;
    return error.empty();
}

bool applyDatabaseUsageRequestJson(const std::string& body,
                                   DatabaseUsageRequest& request,
                                   std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    int intValue = 0;
    bool boolValue = false;
    if (getStringField(values, {"dbPath", "db"}, text, error)) request.dbPath = text;
    if (getIntField(values, {"limit"}, intValue, error)) request.limit = std::max(1, intValue);
    if (getBoolField(values, {"showUnused", "unused"}, boolValue, error)) request.showUnused = boolValue;
    return error.empty();
}

bool applyDatabaseUsageExportRequestJson(const std::string& body,
                                         DatabaseUsageExportRequest& request,
                                         std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    bool boolValue = false;
    if (getStringField(values, {"dbPath", "db"}, text, error)) request.dbPath = text;
    if (getStringField(values, {"outputDir", "output"}, text, error)) request.outputDir = text;
    if (getBoolField(values, {"confirm"}, boolValue, error)) request.confirm = boolValue;
    return error.empty();
}

bool applyDatabasePurgeRequestJson(const std::string& body,
                                   DatabasePurgeRequest& request,
                                   std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    bool boolValue = false;
    if (getStringField(values, {"dbPath", "db"}, text, error)) request.dbPath = text;
    if (getBoolField(values, {"dryRun"}, boolValue, error)) request.dryRun = boolValue;
    if (getBoolField(values, {"confirm"}, boolValue, error)) request.confirm = boolValue;
    return error.empty();
}

bool applyInspectRequestJson(const std::string& body,
                             InspectRequest& request,
                             std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    if (getStringField(values, {"imagePath", "input"}, text, error)) request.imagePath = text;
    if (getStringField(values, {"dbPath", "db"}, text, error)) request.dbPath = text;
    return error.empty();
}

} // namespace mosaicraft
