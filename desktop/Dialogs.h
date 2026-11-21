#pragma once

#include <QSize>
#include <QString>

class QSettings;
class QWidget;

enum class SettingsPage { General, Preferences, Language, Library };

struct LibraryBuildOptions
{
    QSize size{180, 320};
    bool recursive = true;
    int jpegQuality = 100;
    int pngLevel = 9;
    int webpQuality = 0;
};

bool editSettings(QWidget *parent, QSettings &settings, SettingsPage page);
LibraryBuildOptions libraryBuildOptions(const QSettings &settings);
void editLibrarySettings(QWidget *parent, QSettings &settings);
