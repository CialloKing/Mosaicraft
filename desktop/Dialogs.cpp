#include "Dialogs.h"
#include "ui_LibrarySettingsDialog.h"
#include "ui_SettingsDialog.h"

#include <QCoreApplication>
#include <QFileDialog>
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

void populateLibrarySettings(Ui::LibrarySettingsDialog &form, const QSettings &settings)
{
    const auto options = libraryBuildOptions(settings);
    form.normalizeWidth->setValue(options.size.width());
    form.normalizeHeight->setValue(options.size.height());
    form.recursiveImport->setChecked(options.recursive);
    form.libraryJpegQuality->setValue(options.jpegQuality);
    form.libraryPngLevel->setValue(options.pngLevel);
    form.libraryWebpQuality->setValue(options.webpQuality);
}

void saveLibrarySettings(const Ui::LibrarySettingsDialog &form, QSettings &settings)
{
    settings.setValue("library/width", form.normalizeWidth->value());
    settings.setValue("library/height", form.normalizeHeight->value());
    settings.setValue("library/recursive", form.recursiveImport->isChecked());
    settings.setValue("library/jpegQuality", form.libraryJpegQuality->value());
    settings.setValue("library/pngLevel", form.libraryPngLevel->value());
    settings.setValue("library/webpQuality", form.libraryWebpQuality->value());
}
}

bool editSettings(QWidget *parent, QSettings &settings, SettingsPage page)
{
    QDialog dialog(parent);
    Ui::SettingsDialog form;
    form.setupUi(&dialog);
    auto libraryPage = new QDialog(form.settingsPages);
    Ui::LibrarySettingsDialog libraryForm;
    libraryForm.setupUi(libraryPage);
    libraryPage->setWindowFlags(Qt::Widget);
    libraryForm.librarySettingsButtons->hide();
    libraryForm.librarySettingsLayout->setContentsMargins(9, 9, 9, 9);
    form.settingsPages->addWidget(libraryPage);
    populateLibrarySettings(libraryForm, settings);
    form.cliExecutable->setText(settings.value("general/cli").toString());
    form.defaultOutputFolder->setText(settings.value("general/outputFolder").toString());
    form.recentLimit->setValue(settings.value("general/recentLimit", 10).toInt());
    chooseData(form.defaultBackend, {"auto", "cpu", "cuda", "vulkan", "metal"},
               settings.value("preferences/backend", "auto").toString());
    form.defaultDevice->setText(settings.value("preferences/device").toString());
    form.defaultThreads->setValue(settings.value("preferences/threads", 0).toInt());
    form.confirmOverwrite->setChecked(settings.value("preferences/confirmOverwrite", true).toBool());
    chooseData(form.interfaceLanguage, {"system", "zh_CN", "en"}, settings.value("language/locale", "zh_CN").toString());

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
        saveLibrarySettings(libraryForm, settings);
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

void editLibrarySettings(QWidget *parent, QSettings &settings)
{
    QDialog dialog(parent);
    Ui::LibrarySettingsDialog form;
    form.setupUi(&dialog);
    populateLibrarySettings(form, settings);
    connectButtons(dialog, form.librarySettingsButtons);
    if (dialog.exec() == QDialog::Accepted)
    {
        saveLibrarySettings(form, settings);
    }
}
