#include "ApiRequestParser.h"

#include "ApiMetadata.h"
#include "json.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>

namespace mosaicraft
{

namespace
{

using json = nlohmann::json;

using FieldAliases = std::unordered_map<std::string, std::vector<std::string>>;

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
                    const std::vector<std::string>& keys,
                    std::string& out,
                    std::string& error)
{
    if (!error.empty()) return false;
    for (const auto& key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_string()) {
            error = key + " must be a string";
            return false;
        }
        out = it->get<std::string>();
        return true;
    }
    return false;
}

bool getIntField(const json& body,
                 const std::vector<std::string>& keys,
                 int& out,
                 std::string& error)
{
    if (!error.empty()) return false;
    for (const auto& key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_number()) {
            error = key + " must be a number";
            return false;
        }
        out = static_cast<int>(it->get<double>());
        return true;
    }
    return false;
}

bool getDoubleField(const json& body,
                    const std::vector<std::string>& keys,
                    double& out,
                    std::string& error)
{
    if (!error.empty()) return false;
    for (const auto& key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_number()) {
            error = key + " must be a number";
            return false;
        }
        out = it->get<double>();
        return true;
    }
    return false;
}

bool getBoolField(const json& body,
                  const std::vector<std::string>& keys,
                  bool& out,
                  std::string& error)
{
    if (!error.empty()) return false;
    for (const auto& key : keys) {
        auto it = body.find(key);
        if (it == body.end() || it->is_null()) continue;
        if (!it->is_boolean()) {
            error = key + " must be a boolean";
            return false;
        }
        out = it->get<bool>();
        return true;
    }
    return false;
}

bool hasQuery(const ApiQueryParams& query, const std::string& key)
{
    return query.find(key) != query.end();
}

std::string getQuery(const ApiQueryParams& query, const std::string& key)
{
    auto it = query.find(key);
    return it == query.end() ? std::string() : it->second;
}

FieldAliases aliasesFor(ApiOperation operation)
{
    for (const auto& endpoint : apiEndpointMetadata(false)) {
        if (endpoint.operation == operation) return endpoint.fieldAliases;
    }
    return {};
}

std::vector<std::string> keysFor(const FieldAliases& aliases, const std::string& field)
{
    std::vector<std::string> keys{field};
    auto it = aliases.find(field);
    if (it != aliases.end()) {
        keys.insert(keys.end(), it->second.begin(), it->second.end());
    }
    return keys;
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
    const auto aliases = aliasesFor(ApiOperation::Mosaic);

    if (getStringField(values, keysFor(aliases, "inputPath"), text, error))
        request.inputPath = text;
    if (getStringField(values, keysFor(aliases, "dbPath"), text, error))
        request.dbPath = text;
    if (getStringField(values, keysFor(aliases, "outputPath"), text, error))
        request.outputPath = text;

    auto& cfg = request.config;
    if (getIntField(values, keysFor(aliases, "tileW"), intValue, error)) cfg.tileW = std::max(4, intValue);
    if (getIntField(values, keysFor(aliases, "tileH"), intValue, error)) cfg.tileH = std::max(4, intValue);
    if (getIntField(values, keysFor(aliases, "outW"), intValue, error)) cfg.outW = intValue > 0 ? intValue : 0;
    if (getIntField(values, keysFor(aliases, "outH"), intValue, error)) cfg.outH = intValue > 0 ? intValue : 0;
    if (getIntField(values, keysFor(aliases, "nativeTileW"), intValue, error)) cfg.nativeTileW = std::max(1, intValue);
    if (getIntField(values, keysFor(aliases, "nativeTileH"), intValue, error)) cfg.nativeTileH = std::max(1, intValue);
    if (getIntField(values, keysFor(aliases, "candidates"), intValue, error)) cfg.candidates = std::max(10, intValue);
    if (getIntField(values, keysFor(aliases, "topNrandom"), intValue, error))
        cfg.topNrandom = std::max(1, intValue);
    if (getIntField(values, keysFor(aliases, "neighborWindow"), intValue, error)) cfg.neighborWindow = intValue;
    if (getIntField(values, keysFor(aliases, "upscale"), intValue, error)) cfg.upscale = std::max(1, intValue);
    if (getIntField(values, keysFor(aliases, "quality"), intValue, error))
        cfg.jpegQuality = std::max(1, std::min(100, intValue));
    if (getIntField(values, keysFor(aliases, "pngLevel"), intValue, error))
        cfg.pngCompressionLevel = std::max(1, std::min(9, intValue));

    if (getDoubleField(values, keysFor(aliases, "lRange"), doubleValue, error)) cfg.lRange = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "usePenalty"), doubleValue, error))
        cfg.usePenalty = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "labWeight"), doubleValue, error)) cfg.labWeight = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "gridWeight"), doubleValue, error)) cfg.gridWeight = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "tinyWeight"), doubleValue, error)) cfg.tinyWeight = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "edgeWeight"), doubleValue, error)) cfg.edgeWeight = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "lbpWeight"), doubleValue, error)) cfg.lbpWeight = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "neighborPenalty"), doubleValue, error)) cfg.neighborPenalty = doubleValue;
    if (getDoubleField(values, keysFor(aliases, "colorStrength"), doubleValue, error))
        cfg.colorStrength = std::max(0.0, std::min(0.5, doubleValue));

    if (getStringField(values, keysFor(aliases, "format"), text, error)) {
        if (!text.empty()) {
            cfg.outputFormat = text;
            cfg.formatExplicit = true;
        }
    }
    if (getStringField(values, keysFor(aliases, "writeMode"), text, error)) {
        if (text == "auto" || text == "stream" || text == "batch") {
            cfg.writeMode = text;
        } else {
            error = "writeMode must be auto, stream, or batch";
            return false;
        }
    }
    if (getStringField(values, keysFor(aliases, "outputTile"), text, error)) {
        if (!parseSize(text, cfg.nativeTileW, cfg.nativeTileH)) {
            error = "outputTile must use WxH format";
            return false;
        }
    }

    if (getBoolField(values, keysFor(aliases, "useGpu"), boolValue, error)) cfg.useGpu = boolValue;
    if (getBoolField(values, keysFor(aliases, "cpu"), boolValue, error) && boolValue) cfg.useGpu = false;
    if (getBoolField(values, keysFor(aliases, "tiled"), boolValue, error))
        cfg.tiledOutput = boolValue;
    if (getBoolField(values, keysFor(aliases, "deepZoom"), boolValue, error)) {
        cfg.deepZoom = boolValue;
        if (boolValue) cfg.tiledOutput = true;
    }
    if (getBoolField(values, keysFor(aliases, "colorAdjust"), boolValue, error)) cfg.colorAdjust = boolValue;
    if (getBoolField(values, keysFor(aliases, "adaptiveWeights"), boolValue, error)) cfg.adaptiveWeights = boolValue;
    if (getBoolField(values, keysFor(aliases, "analyze"), boolValue, error)) cfg.analyze = boolValue;
    if (getBoolField(values, keysFor(aliases, "benchmark"), boolValue, error)) cfg.benchmark = boolValue;

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
    const auto aliases = aliasesFor(ApiOperation::SubmitBuildJob);

    if (getStringField(values, keysFor(aliases, "inputDir"), text, error))
        request.inputDir = text;
    if (getStringField(values, keysFor(aliases, "outputDir"), text, error))
        request.outputDir = text;
    if (getStringField(values, keysFor(aliases, "dbPath"), text, error))
        request.dbPath = text;
    if (getStringField(values, keysFor(aliases, "normalizeSize"), text, error)) {
        if (!parseSize(text, request.normalizeWidth, request.normalizeHeight)) {
            error = "normalizeSize must use WxH format";
            return false;
        }
    }
    if (getIntField(values, keysFor(aliases, "threads"), intValue, error))
        request.threads = std::max(0, intValue);
    if (getIntField(values, keysFor(aliases, "normalizeWidth"), intValue, error))
        request.normalizeWidth = std::max(1, intValue);
    if (getIntField(values, keysFor(aliases, "normalizeHeight"), intValue, error))
        request.normalizeHeight = std::max(1, intValue);

    if (getBoolField(values, keysFor(aliases, "appendMode"), boolValue, error))
        request.appendMode = boolValue;
    if (getBoolField(values, keysFor(aliases, "normalizeOnly"), boolValue, error))
        request.normalizeOnly = boolValue;
    if (getBoolField(values, keysFor(aliases, "forceMode"), boolValue, error))
        request.forceMode = boolValue;
    if (getBoolField(values, keysFor(aliases, "recursive"), boolValue, error))
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
    const auto aliases = aliasesFor(ApiOperation::DatabaseStats);
    if (getStringField(values, keysFor(aliases, "dbPath"), text, error)) request.dbPath = text;
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
    const auto aliases = aliasesFor(ApiOperation::DatabaseUsage);
    if (getStringField(values, keysFor(aliases, "dbPath"), text, error)) request.dbPath = text;
    if (getIntField(values, keysFor(aliases, "limit"), intValue, error)) request.limit = std::max(1, intValue);
    if (getBoolField(values, keysFor(aliases, "showUnused"), boolValue, error)) request.showUnused = boolValue;
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
    const auto aliases = aliasesFor(ApiOperation::DatabaseUsageExport);
    if (getStringField(values, keysFor(aliases, "dbPath"), text, error)) request.dbPath = text;
    if (getStringField(values, keysFor(aliases, "outputDir"), text, error)) request.outputDir = text;
    if (getBoolField(values, keysFor(aliases, "confirm"), boolValue, error)) request.confirm = boolValue;
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
    const auto aliases = aliasesFor(ApiOperation::DatabasePurge);
    if (getStringField(values, keysFor(aliases, "dbPath"), text, error)) request.dbPath = text;
    if (getBoolField(values, keysFor(aliases, "dryRun"), boolValue, error)) request.dryRun = boolValue;
    if (getBoolField(values, keysFor(aliases, "confirm"), boolValue, error)) request.confirm = boolValue;
    return error.empty();
}

bool applyInspectRequestJson(const std::string& body,
                             InspectRequest& request,
                             std::string& error)
{
    json values;
    if (!parseJsonObject(body, values, error)) return false;

    std::string text;
    const auto aliases = aliasesFor(ApiOperation::Inspect);
    if (getStringField(values, keysFor(aliases, "imagePath"), text, error)) request.imagePath = text;
    if (getStringField(values, keysFor(aliases, "dbPath"), text, error)) request.dbPath = text;
    return error.empty();
}

bool parseDatabaseRequestApi(const ApiQueryParams& query,
                             const std::string& body,
                             DatabaseRequest& request,
                             std::string& error)
{
    if (hasQuery(query, "db")) request.dbPath = getQuery(query, "db");
    return applyDatabaseRequestJson(body, request, error);
}

bool parseDatabaseUsageRequestApi(const ApiQueryParams& query,
                                  const std::string& body,
                                  DatabaseUsageRequest& request,
                                  std::string& error)
{
    if (hasQuery(query, "db")) request.dbPath = getQuery(query, "db");
    if (hasQuery(query, "limit")) request.limit = std::max(1, std::atoi(getQuery(query, "limit").c_str()));
    if (hasQuery(query, "unused")) request.showUnused = getQuery(query, "unused") == "1";
    return applyDatabaseUsageRequestJson(body, request, error);
}

bool parseDatabaseUsageExportRequestApi(const ApiQueryParams& query,
                                        const std::string& body,
                                        DatabaseUsageExportRequest& request,
                                        std::string& error)
{
    if (hasQuery(query, "db")) request.dbPath = getQuery(query, "db");
    if (hasQuery(query, "output")) request.outputDir = getQuery(query, "output");
    if (hasQuery(query, "confirm")) request.confirm = getQuery(query, "confirm") == "1";
    return applyDatabaseUsageExportRequestJson(body, request, error);
}

bool parseDatabasePurgeRequestApi(const ApiQueryParams& query,
                                  const std::string& body,
                                  DatabasePurgeRequest& request,
                                  std::string& error)
{
    if (hasQuery(query, "db")) request.dbPath = getQuery(query, "db");
    if (hasQuery(query, "dryRun")) request.dryRun = getQuery(query, "dryRun") != "0";
    if (hasQuery(query, "confirm")) request.confirm = getQuery(query, "confirm") == "1";
    return applyDatabasePurgeRequestJson(body, request, error);
}

bool parseInspectRequestApi(const ApiQueryParams& query,
                            const std::string& body,
                            InspectRequest& request,
                            std::string& error)
{
    if (hasQuery(query, "input")) request.imagePath = getQuery(query, "input");
    if (hasQuery(query, "db")) request.dbPath = getQuery(query, "db");
    return applyInspectRequestJson(body, request, error);
}

} // namespace mosaicraft
