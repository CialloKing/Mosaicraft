#include "Dialogs.h"
#include "LibraryLocation.h"
#include "ui_SettingsDialog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QRegularExpression>
#include <QSettings>

namespace
{
void connectButtons(QDialog &dialog, QDialogButtonBox *buttons)
{
    buttons->button(QDialogButtonBox::Save)->setText(QCoreApplication::translate("DesktopDialogs", "保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("DesktopDialogs", "取消"));
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
}

void chooseData(QComboBox *control, const QStringList &values, const QString &value)
{
    for (int index = 0; index < values.size(); ++index)
    {
        control->setItemData(index, values[index]);
    }
    control->setCurrentIndex(qMax(0, values.indexOf(value)));
}

void populateLibrarySettings(Ui::SettingsDialog &form, const QSettings &settings)
{
    const auto options = libraryBuildOptions(settings);
    form.librarySource->setText(QDir::toNativeSeparators(settings.value("library/source").toString()));
    form.libraryDirectory->setText(QDir::toNativeSeparators(settings.value("library/directory").toString()));
    form.libraryDirectory->setPlaceholderText(QDir::toNativeSeparators(defaultLibraryDirectory(settings)));
    form.normalizeWidth->setValue(options.size.width());
    form.normalizeHeight->setValue(options.size.height());
    form.recursiveImport->setChecked(options.recursive);
    form.libraryJpegQuality->setValue(options.jpegQuality);
    form.libraryPngLevel->setValue(options.pngLevel);
    form.libraryWebpQuality->setValue(options.webpQuality);
}

void saveLibrarySettings(const Ui::SettingsDialog &form, QSettings &settings)
{
    const auto source = form.librarySource->text().trimmed();
    const auto directory = form.libraryDirectory->text().trimmed();
    settings.setValue("library/source", source.isEmpty() ? QString{} : QFileInfo(source).absoluteFilePath());
    settings.setValue("library/directory", directory.isEmpty() ? QString{} : QFileInfo(directory).absoluteFilePath());
    settings.setValue("library/width", form.normalizeWidth->value());
    settings.setValue("library/height", form.normalizeHeight->value());
    settings.setValue("library/recursive", form.recursiveImport->isChecked());
    settings.setValue("library/jpegQuality", form.libraryJpegQuality->value());
    settings.setValue("library/pngLevel", form.libraryPngLevel->value());
    settings.setValue("library/webpQuality", form.libraryWebpQuality->value());
}
}

bool editSettings(QWidget *parent, QSettings &settings, SettingsPage page, const QString &libraryStatus)
{
    QDialog dialog(parent);
    Ui::SettingsDialog form;
    form.setupUi(&dialog);
    populateLibrarySettings(form, settings);
    form.libraryStatus->setTextFormat(Qt::PlainText);
    form.libraryStatus->setText(libraryStatus);
    form.libraryStatus->setVisible(!libraryStatus.isEmpty());
    const auto browseDirectory = [&dialog](QPushButton *button, QLineEdit *field, const QString &title) {
        QObject::connect(button, &QPushButton::clicked, &dialog, [&dialog, field, title] {
            const auto initial = field->text().isEmpty() ? field->placeholderText() : field->text();
            const auto folder = QFileDialog::getExistingDirectory(&dialog, title, initial);
            if (!folder.isEmpty()) field->setText(QDir::toNativeSeparators(folder));
        });
    };
    browseDirectory(form.browseLibrarySource, form.librarySource,
                    QCoreApplication::translate("SettingsDialog", "选择原始素材目录"));
    browseDirectory(form.browseLibraryDirectory, form.libraryDirectory,
                    QCoreApplication::translate("SettingsDialog", "选择素材库目录"));
    form.cliExecutable->setText(settings.value("general/cli").toString());
    form.defaultOutputFolder->setText(settings.value("general/outputFolder").toString());
    form.recentLimit->setValue(settings.value("general/recentLimit", 10).toInt());
    chooseData(form.defaultBackend, {"auto", "cpu", "cuda", "vulkan", "metal"},
               settings.value("preferences/backend", "auto").toString());
    form.defaultDevice->setText(settings.value("preferences/device").toString());
    form.defaultThreads->setValue(settings.value("preferences/threads", 0).toInt());
    form.confirmOverwrite->setChecked(settings.value("preferences/confirmOverwrite", true).toBool());
    chooseData(form.interfaceLanguage, {"system", "zh_CN", "en"}, settings.value("language/locale", "zh_CN").toString());
    form.openLogWindow->setChecked(settings.value(openLogWindowSetting).toBool());

    QObject::connect(form.settingsNavigation, &QListWidget::currentRowChanged, form.settingsPages, &QStackedWidget::setCurrentIndex);
    form.settingsNavigation->setCurrentRow(static_cast<int>(page));
    connectButtons(dialog, form.settingsButtons);
    QObject::connect(form.browseExecutable, &QPushButton::clicked, &dialog, [&] {
        const auto filename = QFileDialog::getOpenFileName(&dialog, QCoreApplication::translate("SettingsDialog", "选择本地 Mosaicraft CLI"),
                                                          form.cliExecutable->text());
        if (!filename.isEmpty())
        {
            form.cliExecutable->setText(filename);
        }
    });
    QObject::connect(form.browseDefaultOutput, &QPushButton::clicked, &dialog, [&] {
        const auto folder = QFileDialog::getExistingDirectory(&dialog, QCoreApplication::translate("SettingsDialog", "默认导出目录"),
                                                              form.defaultOutputFolder->text());
        if (!folder.isEmpty())
        {
            form.defaultOutputFolder->setText(folder);
        }
    });

    while (dialog.exec() == QDialog::Accepted)
    {
        const auto backend = form.defaultBackend->currentData().toString();
        const auto device = form.defaultDevice->text().trimmed();
        const auto deviceBackend = device.section(':', 0, 0);
        const QRegularExpression devicePattern("^(?:auto|cpu|(?:cuda|vulkan|metal):[0-9]+)$");
        if (!device.isEmpty() && (!devicePattern.match(device).hasMatch() ||
            (device != "auto" && backend != "auto" && deviceBackend != backend)))
        {
            form.settingsNavigation->setCurrentRow(static_cast<int>(SettingsPage::Preferences));
            form.defaultDevice->setFocus();
            QMessageBox::warning(&dialog, dialog.windowTitle(),
                                 QCoreApplication::translate("SettingsDialog", "设备标识无效，或与所选后端不一致。"));
            continue;
        }
        settings.setValue("general/cli", form.cliExecutable->text().trimmed());
        settings.setValue("general/outputFolder", form.defaultOutputFolder->text().trimmed());
        settings.setValue("general/recentLimit", form.recentLimit->value());
        settings.setValue("preferences/backend", backend);
        settings.setValue("preferences/device", device);
        settings.setValue("preferences/threads", form.defaultThreads->value());
        settings.setValue("preferences/confirmOverwrite", form.confirmOverwrite->isChecked());
        settings.setValue("language/locale", form.interfaceLanguage->currentData().toString());
        settings.setValue(openLogWindowSetting, form.openLogWindow->isChecked());
        saveLibrarySettings(form, settings);
        return true;
    }
    return false;
}

LibraryBuildOptions libraryBuildOptions(const QSettings &settings)
{
    return {{qBound(1, settings.value("library/width", 180).toInt(), 8192),
             qBound(1, settings.value("library/height", 320).toInt(), 8192)},
            settings.value("library/recursive", true).toBool(),
            qBound(1, settings.value("library/jpegQuality", 100).toInt(), 100),
            qBound(0, settings.value("library/pngLevel", 9).toInt(), 9),
            qBound(0, settings.value("library/webpQuality", 0).toInt(), 100)};
}
