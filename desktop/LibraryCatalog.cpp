#include "LibraryCatalog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace
{
QString readDatabase(QSqlDatabase &database, LibraryCatalog &catalog)
{
    database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1500");
    database.setDatabaseName(catalog.databasePath);
    if (!database.open())
    {
        return database.lastError().text();
    }

    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec("PRAGMA user_version") || !query.next() || query.value(0).toInt() != MOSAICRAFT_LIBRARY_VERSION)
    {
        return QCoreApplication::translate("LibraryCatalog", "不支持此图库版本。请选择由当前版本创建的图库。");
    }
    if (!query.exec("SELECT key, value FROM meta WHERE key IN ('feature_w', 'feature_h')"))
    {
        return query.lastError().text();
    }
    while (query.next())
    {
        if (query.value(0).toString() == "feature_w")
        {
            catalog.normalizedSize.setWidth(query.value(1).toInt());
        }
        else
        {
            catalog.normalizedSize.setHeight(query.value(1).toInt());
        }
    }
    if (!query.exec("SELECT id, file_path, src_width, src_height, file_size, created_at FROM images ORDER BY id"))
    {
        return query.lastError().text();
    }
    while (query.next())
    {
        catalog.assets.append({query.value(0).toLongLong(), query.value(1).toString(),
                               {query.value(2).toInt(), query.value(3).toInt()},
                               query.value(4).toLongLong(), query.value(5).toString()});
    }
    return query.lastError().text();
}
}

QString libraryDatabasePath(const QString &path)
{
    const QFileInfo info(path);
    return info.isDir() ? QDir(info.absoluteFilePath()).filePath("mosaicraft.db") : info.absoluteFilePath();
}

bool directoryContains(const QString &directory, const QString &path)
{
    const auto canonicalDirectory = QFileInfo(directory).canonicalFilePath();
    const auto canonicalPath = QFileInfo(path).canonicalFilePath();
    if (canonicalDirectory.isEmpty() || canonicalPath.isEmpty())
    {
        return false;
    }
    const auto relative = QDir(canonicalDirectory).relativeFilePath(canonicalPath);
    return !QDir::isAbsolutePath(relative) && relative != ".." && !relative.startsWith("../");
}

QString containedFile(const QString &directory, const QString &relativePath)
{
    if (relativePath.isEmpty() || relativePath.contains('\\') || relativePath.contains(':') ||
        QDir::isAbsolutePath(relativePath) || QDir::cleanPath(relativePath) != relativePath ||
        relativePath.split('/').contains(".."))
    {
        return {};
    }
    const QFileInfo info(QDir(directory).filePath(relativePath));
    return info.isFile() && directoryContains(directory, info.absoluteFilePath()) ? info.canonicalFilePath() : QString{};
}

QString LibraryCatalog::assetPath(const LibraryAsset &asset) const
{
    return containedFile(root, asset.relativePath);
}

QString readLibrary(const QString &path, LibraryCatalog &result)
{
    LibraryCatalog catalog;
    const QFileInfo file(libraryDatabasePath(path));
    if (!file.isFile())
    {
        return QCoreApplication::translate("LibraryCatalog", "目录中没有 mosaicraft.db。请先导入素材建库。");
    }
    catalog.databasePath = file.canonicalFilePath();
    catalog.root = QFileInfo(catalog.databasePath).absolutePath();

    const auto connection = QUuid::createUuid().toString();
    QString error;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        error = readDatabase(database, catalog);
    }
    QSqlDatabase::removeDatabase(connection);
    if (error.isEmpty())
    {
        result = std::move(catalog);
    }
    return error;
}
