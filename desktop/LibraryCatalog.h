#pragma once

#include <QSize>
#include <QString>
#include <QVector>

struct LibraryAsset
{
    qint64 id = 0;
    QString relativePath;
    QSize sourceSize;
    qint64 bytes = 0;
    QString importedAt;
};

struct LibraryCatalog
{
    QString databasePath;
    QString root;
    QSize normalizedSize;
    QVector<LibraryAsset> assets;
    QString assetPath(const LibraryAsset &asset) const;
};

QString libraryDatabasePath(const QString &path);
bool directoryContains(const QString &directory, const QString &path);
QString containedFile(const QString &directory, const QString &relativePath);
QString readLibrary(const QString &path, LibraryCatalog &result);
