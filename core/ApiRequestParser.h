#pragma once

#include "BuildService.h"
#include "DatabaseService.h"
#include "InspectService.h"
#include "MosaicService.h"

#include <string>
#include <unordered_map>

namespace mosaicraft
{

using ApiQueryParams = std::unordered_map<std::string, std::string>;

bool parseMosaicRequestJson(const std::string& body,
                            MosaicRequest& request,
                            std::string& error);

bool parseBuildRequestJson(const std::string& body,
                           BuildRequest& request,
                           std::string& error);

bool applyDatabaseRequestJson(const std::string& body,
                              DatabaseRequest& request,
                              std::string& error);

bool applyDatabaseUsageRequestJson(const std::string& body,
                                   DatabaseUsageRequest& request,
                                   std::string& error);

bool applyDatabaseUsageExportRequestJson(const std::string& body,
                                         DatabaseUsageExportRequest& request,
                                         std::string& error);

bool applyDatabasePurgeRequestJson(const std::string& body,
                                   DatabasePurgeRequest& request,
                                   std::string& error);

bool applyInspectRequestJson(const std::string& body,
                             InspectRequest& request,
                             std::string& error);

bool parseDatabaseRequestApi(const ApiQueryParams& query,
                             const std::string& body,
                             DatabaseRequest& request,
                             std::string& error);

bool parseDatabaseUsageRequestApi(const ApiQueryParams& query,
                                  const std::string& body,
                                  DatabaseUsageRequest& request,
                                  std::string& error);

bool parseDatabaseUsageExportRequestApi(const ApiQueryParams& query,
                                        const std::string& body,
                                        DatabaseUsageExportRequest& request,
                                        std::string& error);

bool parseDatabasePurgeRequestApi(const ApiQueryParams& query,
                                  const std::string& body,
                                  DatabasePurgeRequest& request,
                                  std::string& error);

bool parseInspectRequestApi(const ApiQueryParams& query,
                            const std::string& body,
                            InspectRequest& request,
                            std::string& error);

} // namespace mosaicraft
