#pragma once

#include <QString>

class QSettings;

QString defaultLibraryDirectory(const QSettings &settings);
QString defaultLibraryDatabase(const QSettings &settings);
QString containedFile(const QString &directory, const QString &relativePath);
