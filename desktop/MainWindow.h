#pragma once

#include "LibraryCatalog.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QMainWindow>
#include <QPixmap>
#include <QSet>
#include <QStringList>

#include <memory>

class QAction;
class QCloseEvent;
class QDragEnterEvent;
class QDropEvent;
class QGraphicsScene;
class QGraphicsView;
class QListWidgetItem;
class QNetworkAccessManager;
class QProcess;
class QSettings;
class QTemporaryDir;

namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QSettings &settings, QWidget *parent = nullptr);
    ~MainWindow() override;
    bool openPath(const QString &path);
    int smokeTest(const QString &captureDirectory);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private:
    enum class Workspace { Edit, Export, Library };
    enum class Job { None, Export, Import, PortablePreview };
    enum class ViewMode { Target, Result, Compare };

    struct Document
    {
        QString file;
        QString manifest;
        QString target;
        QString library;
        QString resultImage;
        QString outputLocation;
        QString resultMessage;
        QSize resultSize;
        QSize projectGrid;
        QSize projectTile;
        QSize previewGrid;
        QPixmap preview, reference;
        QJsonObject previewRequest;
        QJsonArray edits;
        QString layout;
        QJsonObject options;
        int id = -1;
        int selectedTile = -1;
        ViewMode view = ViewMode::Target;
        bool dirty = false;
    };

    struct JobState
    {
        Job type = Job::None;
        int documentIndex = -1;
        QString destination;
        QByteArray response;
        QString diagnostics;
    };

    void setupMenu();
    void setupEditor();
    void setupExport();
    void setupLibrary();
    void switchWorkspace(Workspace workspace);
    void newDocument();
    void addDocument(Document document);
    void showDocument(int index);
    QString documentName(const Document &document) const;
    void updateTab();
    void closeDocument(int index);
    bool confirmClose(int index);
    bool saveDocument(int index, bool saveAs);
    void chooseDocument();
    void rememberFile(const QString &path);
    void updateRecentMenu();
    bool busy() const;
    bool previewCurrent() const;
    void updateEditorControls();
    void setViewMode(ViewMode mode);
    void refreshCanvas(bool preserveView = false);
    void fitCanvas();
    void setZoom(qreal factor, bool fit);
    void placeCanvas();
    void setupPreviewSession();
    void closePreviewSession();
    void generatePreview();
    void selectTile(QGraphicsView *view, const QPoint &point);
    void replaceTile(QListWidgetItem *item);
    void sendEditorRequest(QJsonObject request);
    void receiveEditorReply(const QJsonObject &reply);
    void editorDisconnected(const QString &error);
    void formChanged();
    QJsonObject captureOptions() const;
    void applyOptions(const QJsonObject &options);
    void updateSummaries();
    void updateOutputControls();
    void refreshExportPreview();
    void showExportImage();
    void chooseOutput();
    QString suggestedOutput() const;
    struct ExportOptions;
    void collectExportOptions(ExportOptions &options, const Document &document, const QString &destination,
                              bool preview) const;
    void collectExecutionOptions(ExportOptions &options, bool useDevice) const;
    QJsonObject buildEditorRequest(const Document &document, const QString &destination, bool preview = false) const;
    QStringList buildExportArgs(const Document &document, const QString &destination, bool preview = false) const;
    void exportDocument();
    void runExport(const QString &destination);
    void appendExecutionArgs(QStringList &args, bool useDevice) const;
    void runJob(Job type, const QStringList &args, const QString &destination);
    void finishJob(bool processSucceeded, const QString &failure = {});
    QString resolveCli() const;
    QString resolveEditorWorker() const;
    void showAbout();
    void checkUpdates();
    void chooseLibrary();
    bool loadLibrary(const QString &path, bool showError = true);
    void sortAssets();
    void renderAssets();
    const LibraryAsset *activeAsset() const;
    void updateAssetSelection();
    void openAsset();
    QStringList buildLibraryArgs(const QString &source, const QString &destination, bool append) const;
    void importAssets();
    void exportSelectedAssets();
    void useCurrentLibrary();

    std::unique_ptr<Ui::MainWindow> ui;
    QSettings &settings;
    QGraphicsScene *canvasScene;
    QGraphicsScene *compareScene;
    QNetworkAccessManager *network;
    QProcess *process;
    QProcess *editor;
    std::unique_ptr<QTemporaryDir> previewFiles;
    QList<QWidget *> fields;
    QVector<Document> documents;
    QJsonObject defaultOptions;
    LibraryCatalog catalog;
    QSet<qint64> selectedAssets;
    QStringList recentFiles;
    JobState job;
    QPixmap exportImage;
    QPointF canvasCenter;
    QPoint mouseStart;
    QPointF panStart;
    QGraphicsView *dragView = nullptr;
    qreal zoom = 1;
    QByteArray editorCommand, editorResponse;
    QJsonObject editorRequest;
    QString editorOperation, pendingExport;
    int editorOwner = -1;
    int currentCandidate = 0;
    int nextDocumentId = 0;
    int activeDocument = -1;
    bool fitView = true;
};

QPixmap loadPreviewImage(const QString &path, QSize limit, QSize *sourceSize = nullptr);
