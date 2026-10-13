#pragma once

#include <string>

namespace mosaicraft
{

class Database;

struct ColorOrderResult
{
    int computed = 0;
    int renamed = 0;
    double validationMs = 0;
    double sortingMs = 0;
    double writingMs = 0;
};

// 调用者须以 Database::OpenMode::Build 持有独占图库锁。
bool recoverLibraryColorOrder(Database& database, std::string& error);
bool reorderLibraryByColor(Database& database, const std::string& outputDir,
                           ColorOrderResult& result, std::string& error);

} // namespace mosaicraft
