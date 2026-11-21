#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFont>
#include <QLibraryInfo>
#include <QLocale>
#include <QPalette>
#include <QSettings>
#include <QTemporaryDir>
#include <QTranslator>

int main(int argc, char *argv[])
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setApplicationName("Mosaicraft Desktop");
    app.setOrganizationName("Mosaicraft");
    app.setApplicationVersion(MOSAICRAFT_DESKTOP_VERSION);
    app.setStyle("Fusion");

    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#2c2c2c"));
    palette.setColor(QPalette::Base, QColor("#242424"));
    palette.setColor(QPalette::AlternateBase, QColor("#333333"));
    palette.setColor(QPalette::Button, QColor("#383838"));
    palette.setColor(QPalette::WindowText, QColor("#e3e3e3"));
    palette.setColor(QPalette::Text, QColor("#e3e3e3"));
    palette.setColor(QPalette::ButtonText, QColor("#e3e3e3"));
    palette.setColor(QPalette::Highlight, QColor("#595959"));
    palette.setColor(QPalette::HighlightedText, QColor("#f6f6f6"));
    palette.setColor(QPalette::ToolTipBase, QColor("#404040"));
    palette.setColor(QPalette::ToolTipText, QColor("#e3e3e3"));
    palette.setColor(QPalette::PlaceholderText, QColor("#a3a3a3"));
    palette.setColor(QPalette::Link, QColor("#d0d0d0"));
    palette.setColor(QPalette::LinkVisited, QColor("#b8b8b8"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#858585"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#858585"));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#858585"));
    app.setPalette(palette);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "Desktop project (.mscproj), portable project directory or target image.");
    parser.addOption({"smoke-test", "Run local UI smoke checks without user settings."});
    parser.addOption({"capture-dir", "Save smoke-test workspace captures in this folder.", "directory"});
    parser.process(app);
    const bool smoke = parser.isSet("smoke-test") || parser.isSet("capture-dir");
    std::unique_ptr<QTemporaryDir> scratch;
    if (smoke)
    {
        scratch = std::make_unique<QTemporaryDir>();
        if (!scratch->isValid())
        {
            return 1;
        }
    }
    auto settings = smoke
        ? std::make_unique<QSettings>(QDir(scratch->path()).filePath("smoke.ini"), QSettings::IniFormat)
        : std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope, app.organizationName(), app.applicationName());
    const auto language = settings->value("language/locale", "zh_CN").toString();
    const bool english = language == "en" || (language == "system" && QLocale::system().language() != QLocale::Chinese);
#ifdef Q_OS_WIN
    app.setFont(QFont(english ? "Segoe UI" : "Microsoft YaHei UI", 10));
#endif
    const auto translations = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    QTranslator translator;
    const bool loaded = english ? translator.load(":/i18n/mosaicraft_en.qm")
                                : translator.load(":/i18n/qtbase_zh_CN.qm") ||
                                  translator.load("qtbase_zh_CN", translations) ||
                                  translator.load("qt_zh_CN", translations);
    if (loaded)
    {
        app.installTranslator(&translator);
    }

    MainWindow window(*settings);
    window.show();
    if (smoke)
    {
        return window.smokeTest(parser.value("capture-dir"));
    }
    if (!parser.positionalArguments().isEmpty())
    {
        window.openPath(parser.positionalArguments().front());
    }
    return app.exec();
}
