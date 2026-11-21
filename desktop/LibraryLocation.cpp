#include "LibraryLocation.h"
#include "core/ManagedLibrary.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

using namespace mosaicraft::managed_library;

QString defaultLibraryDirectory(const QSettings &settings)
{
    const auto configured = settings.value("library/directory").toString();
    if (!configured.isEmpty()) return QFileInfo(configured).absoluteFilePath();

    const QDir appData(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation));
    const auto library = appData.filePath("library");
    const QDir legacy(QDir::current().absoluteFilePath("library"));
    if (settings.value("library/source").toString().isEmpty() && !QFileInfo::exists(library) &&
        QFileInfo(legacy.filePath(databaseFile)).isFile())
    {
        return legacy.absolutePath();
    }
    return library;
}

QString defaultLibraryDatabase(const QSettings &settings)
{
    const QDir library(defaultLibraryDirectory(settings));
    const QDir managed(library.filePath(managedDirectory));
    QFile pointer(managed.filePath(activeLibraryFile));
    if (pointer.open(QIODevice::ReadOnly))
    {
        const auto name = pointer.read(64);
        if (isLibraryGeneration(name.toStdString()))
        {
            return managed.filePath(QString::fromLatin1(name) + '/' + databaseFile);
        }
    }
    const auto existing = library.filePath(databaseFile);
    return QFileInfo(existing).isFile() ? existing : QString{};
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
    const auto root = QFileInfo(directory).canonicalFilePath();
    const auto path = info.canonicalFilePath();
    if (!info.isFile() || root.isEmpty() || path.isEmpty()) return {};
    const auto relative = QDir(root).relativeFilePath(path);
    return !QDir::isAbsolutePath(relative) && relative != ".." && !relative.startsWith("../") ? path : QString{};
}
