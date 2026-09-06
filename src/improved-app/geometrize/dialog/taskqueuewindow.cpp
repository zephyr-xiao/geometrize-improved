#include "taskqueuewindow.h"
#include "ui_taskqueuewindow.h"

#include <QCollator>
#include <QDebug>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <vector>

#include "chaiscript/chaiscript.hpp"

#include "common/formatsupport.h"
#include "common/util.h"
#include "dialog/imagetaskwindow.h"
#include "dialog/recenttaskslist.h"
#include "dialog/scripteditorwidget.h"
#include "dialog/taskitemwidget.h"
#include "geometrize/bitmap/bitmap.h"
#include "exporter/imageexporter.h"
#include "geometrize/exporter/svgexporter.h"
#include "geometrize/shaperesult.h"
#include "task/imagetask.h"
#include "script/chaiscriptcreator.h"
#include "script/scriptrunner.h"
#include "script/scriptutil.h"
#include "task/taskutil.h"

#ifdef Q_OS_WIN
#include <objidl.h>
#include <shobjidl.h>
#endif

namespace geometrize
{

namespace dialog
{

namespace
{

#ifdef Q_OS_WIN
// 任务栏进度(Win32 原生 COM,不引 QtWinExtras):批处理跑批时在任务栏图标上叠加进度条。
// COM 初始化是进程级共享状态,Qt 平台插件已在主线程 OleInitialize;这里防御式补充,
// 返回 S_FALSE/RPC_E_CHANGED_MODE 都视为可用且不配对 Uninitialize(避免减掉 Qt 的引用计数)。
class TaskbarProgress
{
public:
    TaskbarProgress()
    {
        const HRESULT coInit{::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)};
        const bool comAvailable{SUCCEEDED(coInit) || coInit == RPC_E_CHANGED_MODE};
        if(!comAvailable) {
            return;
        }
        m_comOwned = SUCCEEDED(coInit);
        const HRESULT hr{::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, reinterpret_cast<void**>(&m_taskbar))};
        if(FAILED(hr)) {
            m_taskbar = nullptr;
        }
    }

    ~TaskbarProgress()
    {
        if(m_taskbar != nullptr) {
            m_taskbar->Release();
        }
        // 显式 CoUninitialize 只在本次调用真正初始化成功时做;
        // S_FALSE(已有其他所有者)时不需要也无法安全配对
        if(m_comOwned) {
            ::CoUninitialize();
        }
    }

    void setValue(HWND window, std::uint32_t completed, std::uint32_t total)
    {
        if(m_taskbar == nullptr || window == nullptr) {
            return;
        }
        m_taskbar->SetProgressState(window, total > 0 ? TBPF_NORMAL : TBPF_NOPROGRESS);
        if(total > 0) {
            m_taskbar->SetProgressValue(window, completed, total);
        }
    }

    void clear(HWND window)
    {
        if(m_taskbar == nullptr || window == nullptr) {
            return;
        }
        m_taskbar->SetProgressState(window, TBPF_NOPROGRESS);
    }

private:
    ITaskbarList3* m_taskbar{nullptr};
    bool m_comOwned{false};
};

#endif

}

class TaskQueueWindow::TaskQueueWindowImpl
{
public:
    TaskQueueWindowImpl(TaskQueueWindow* pQ) :
        ui{std::make_unique<Ui::TaskQueueWindow>()},
        q{pQ}
    {
        ui->setupUi(q);
        populateUi();
        q->setAttribute(Qt::WA_DeleteOnClose);

        refreshScripts();
        setupScriptEditor();

        connect(ui->scriptSelectComboBox, &QComboBox::currentTextChanged, [this](const QString& text) {
            m_scriptEditorWidget->setCurrentCode(m_scripts[text.toStdString()]);
            persistQueue();
        });

        restoreQueue();
        loadAutoExportConfig();

        connect(ui->runTasksButton, &QPushButton::clicked, [this]() {
            resetBatchProgress();
            for(int i = 0; i < ui->taskList->count(); ++i) {
                QListWidgetItem* item = ui->taskList->item(i);
                QString data = item->data(Qt::UserRole).toString();
                runScript(data.toStdString());
            }
        });
        connect(ui->clearTaskListButton, &QPushButton::clicked, [this]() {
            ui->taskList->clear();
            resetBatchProgress();
            persistQueue();
        });
        connect(ui->closeOpenWindowsButton, &QPushButton::clicked, [this]() {
            closeOpenWindows();
        });

#ifdef Q_OS_WIN
        m_taskbar = std::make_unique<TaskbarProgress>();
#endif

        // Enable the run and clear buttons only when items are present
        ui->runTasksButton->setEnabled(false);
        ui->clearTaskListButton->setEnabled(false);
        QTimer* timer = new QTimer(q);
        q->connect(timer, &QTimer::timeout, q, [this]() {
            const bool enableRunClearButtons = ui->taskList->count() > 0;
            ui->runTasksButton->setEnabled(enableRunClearButtons);
            ui->clearTaskListButton->setEnabled(enableRunClearButtons);
            ui->closeOpenWindowsButton->setEnabled(!geometrize::dialog::ImageTaskWindow::getExistingImageTaskWindows().empty());

            // 批处理进度归集:轮询只负责展示与销毁兜底,完成事实由信号驱动。
            // 受管窗口销毁(WA_DeleteOnClose 手动关闭等)视为完成但不触发导出。
            bool dirty = false;
            for(int i = m_trackedWindows.size() - 1; i >= 0; --i) {
                const QPointer<ImageTaskWindow>& window = m_trackedWindows[i];
                if(!window.isNull()) {
                    continue;
                }
                if(!m_completedWindows.contains(window)) {
                    m_completedWindows.push_back(window); // 占位计数:销毁窗口已计入完成
                    dirty = true;
                }
            }
            if(dirty) {
                removeWindowImagePath(QPointer<ImageTaskWindow>());
                m_trackedWindows.erase(std::remove_if(m_trackedWindows.begin(), m_trackedWindows.end(),
                    [](const QPointer<ImageTaskWindow>& w) { return w.isNull(); }), m_trackedWindows.end());
            }

            const std::uint32_t completed{static_cast<std::uint32_t>(m_completedWindows.size())};
            const std::uint32_t total{static_cast<std::uint32_t>(m_trackedWindows.size())};
            q->setWindowTitle(tr("Task Queue (%1/%2)").arg(completed).arg(total));
#ifdef Q_OS_WIN
            m_taskbar->setValue(reinterpret_cast<HWND>(q->winId()), completed, total);
#endif
        });
        timer->start(500);

        connect(ui->autoExportBrowseButton, &QPushButton::clicked, [this]() {
            const QString dir{QFileDialog::getExistingDirectory(q, tr("Select Output Directory"), ui->autoExportDirEdit->text())};
            if(dir.isEmpty()) {
                return;
            }
            ui->autoExportDirEdit->setText(QDir::fromNativeSeparators(dir) + "/");
            saveAutoExportConfig();
        });
        connect(ui->autoExportGroupBox, &QGroupBox::toggled, [this](bool) { saveAutoExportConfig(); });
        connect(ui->autoExportPngCheckBox, &QCheckBox::toggled, [this](bool) { saveAutoExportConfig(); });
        connect(ui->autoExportSvgCheckBox, &QCheckBox::toggled, [this](bool) { saveAutoExportConfig(); });
        connect(ui->autoExportDirEdit, &QLineEdit::editingFinished, [this]() { saveAutoExportConfig(); });
    }
    TaskQueueWindowImpl& operator=(const TaskQueueWindowImpl&) = delete;
    TaskQueueWindowImpl(const TaskQueueWindowImpl&) = delete;
    ~TaskQueueWindowImpl()
    {
#ifdef Q_OS_WIN
        m_taskbar->clear(reinterpret_cast<HWND>(q->winId()));
#endif
    }

    void addItems(const QStringList& tasks)
    {
        for(const auto& task : tasks) {
            addItem(task, task);
        }
    }

    void close()
    {
        q->close();
    }

    void onLanguageChange()
    {
        ui->retranslateUi(q);
        populateUi();
    }

    void closeOpenWindows()
    {
        const auto& windows = geometrize::dialog::ImageTaskWindow::getExistingImageTaskWindows();
        for(auto* window : windows) {
            window->close();
        }
    }

private:
    void refreshScripts()
    {
        m_scripts = geometrize::script::getTaskQueueBatchProcessingScripts();

        ui->scriptSelectComboBox->clear();
        for(const auto& script : m_scripts) {
            ui->scriptSelectComboBox->addItem(QString::fromStdString(script.first));
        }
    }

    void runScript(const std::string& imagePath)
    {
        const auto engine = script::createBatchImageTaskEngine();
        engine->set_global(chaiscript::var(imagePath), "inputPath");
        const auto& code = m_scriptEditorWidget->getCurrentCode();
        const std::vector<ImageTaskWindow*> before{ImageTaskWindow::getExistingImageTaskWindows()};
        try {
            engine->eval(code);
            m_scriptEditorWidget->onScriptEvaluationSucceeded();
        } catch(const chaiscript::exception::eval_error& e) {
            m_scriptEditorWidget->onScriptEvaluationFailed(e.pretty_print());
        } catch(...) {
            m_scriptEditorWidget->onScriptEvaluationFailed("Unknown script evaluation error");
        }
        // 成功与异常路径都要接管新窗口:脚本中途抛错时前面已创建的窗口仍在跑,不能丢
        connectNewTaskWindows(before, QString::fromStdString(imagePath));
    }

    void addItem(const QString& itemPath, const QString& itemDisplayName)
    {
        class MyListWidgetItem : public QListWidgetItem {
        public:
            MyListWidgetItem()
            {
                static int instanceCounter = 0;
                m_instanceId = instanceCounter;
                instanceCounter++;
                //setData(Qt::DisplayRole, instanceCounter);
            }

            virtual bool operator<(const MyListWidgetItem& other) const {
                return m_instanceId > other.m_instanceId;
            }

        private:
            int m_instanceId;
        };

        QListWidgetItem* item{new MyListWidgetItem()};
        dialog::TaskItemWidget* button{new dialog::TaskItemWidget(itemPath, RecentTasksList::getDisplayNameForTaskPath(itemDisplayName),
        [this](const QString& taskItemId) {
            resetBatchProgress();
            runScript(taskItemId.toStdString());
        },
        [item, this](const QString& /*taskItemId*/) {
            delete item;
            persistQueue();
        })};
        item->setToolTip(itemPath);
        item->setSizeHint(button->sizeHint());
        setMenuItemKey(item, itemPath);
        ui->taskList->addItem(item);
        ui->taskList->setItemWidget(item, button);
        persistQueue();
    }

    void setMenuItemKey(QListWidgetItem* item, const QString& key) const
    {
        item->setData(Qt::UserRole, key);
    }

    void setupScriptEditor()
    {
        const std::string scriptEditorWidgetTitle = tr("Script Editor").toStdString();

        m_scriptEditorWidget = new geometrize::dialog::ScriptEditorWidget(scriptEditorWidgetTitle, defaultScriptName, m_scripts.at(defaultScriptName), ui->taskListScriptContainer);

        connect(m_scriptEditorWidget, &ScriptEditorWidget::signal_scriptChanged, [this](ScriptEditorWidget* /*self*/, const std::string& functionName, const std::string& code) {
            emit q->signal_scriptChanged(functionName, code);
        });

        ui->scrollArea->setWidget(m_scriptEditorWidget);
    }

    // ---- 批处理完成感知:eval 前快照窗口列表,eval 后 diff 出新窗口逐一接管 ----

    void connectNewTaskWindows(const std::vector<ImageTaskWindow*>& before, const QString& imagePath)
    {
        const std::vector<ImageTaskWindow*> after{ImageTaskWindow::getExistingImageTaskWindows()};
        for(ImageTaskWindow* window : after) {
            if(std::find(before.begin(), before.end(), window) != before.end()) {
                continue;
            }
            if(std::find(m_trackedWindows.begin(), m_trackedWindows.end(), QPointer<ImageTaskWindow>(window)) != m_trackedWindows.end()) {
                continue;
            }
            // receiver 必须传 q:任务窗口(WA_DeleteOnClose)销毁时 Qt 自动断连,回调不悬空
            QPointer<ImageTaskWindow> guard{window};
            connect(window, &ImageTaskWindow::signal_didStopConditionMet, q, [this, guard]() {
                onTaskWindowFinished(guard);
            });
            m_trackedWindows.push_back(guard);
            m_windowImagePath.push_back({guard, imagePath});
        }
    }

    QString windowImagePath(const QPointer<ImageTaskWindow>& window) const
    {
        for(const auto& entry : m_windowImagePath) {
            if(entry.first == window) {
                return entry.second;
            }
        }
        return QStringLiteral("task");
    }

    void removeWindowImagePath(const QPointer<ImageTaskWindow>& window)
    {
        m_windowImagePath.erase(std::remove_if(m_windowImagePath.begin(), m_windowImagePath.end(),
            [&window](const QPair<QPointer<ImageTaskWindow>, QString>& entry) {
                return entry.first == window || entry.first.isNull();
            }), m_windowImagePath.end());
    }

    void onTaskWindowFinished(const QPointer<ImageTaskWindow>& window)
    {
        if(m_completedWindows.contains(window)) {
            return;
        }
        m_completedWindows.push_back(window);
        exportTaskResults(window);
    }

    void resetBatchProgress()
    {
        m_trackedWindows.clear();
        m_completedWindows.clear();
        m_exportNameCounter.clear();
        m_windowImagePath.clear();
        q->setWindowTitle(tr("Task Queue (0/0)"));
#ifdef Q_OS_WIN
        m_taskbar->setValue(reinterpret_cast<HWND>(q->winId()), 0, 0);
#endif
    }

    // ---- 自动导出:任务完成后把最终位图/矢量形状写出到配置目录 ----

    struct AutoExportConfig
    {
        bool enabled{false};
        QString directory;
        bool png{true};
        bool svg{false};
    };

    AutoExportConfig autoExportConfig() const
    {
        AutoExportConfig config;
        config.enabled = ui->autoExportGroupBox->isChecked();
        config.directory = ui->autoExportDirEdit->text();
        config.png = ui->autoExportPngCheckBox->isChecked();
        config.svg = ui->autoExportSvgCheckBox->isChecked();
        return config;
    }

    void loadAutoExportConfig()
    {
        QSettings settings;
        settings.beginGroup("task_queue");
        ui->autoExportGroupBox->setChecked(settings.value("auto_export_enabled", false).toBool());
        const QString defaultDir{QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/geometrize_batch_output/"};
        ui->autoExportDirEdit->setText(settings.value("auto_export_dir", defaultDir).toString());
        ui->autoExportPngCheckBox->setChecked(settings.value("auto_export_png", true).toBool());
        ui->autoExportSvgCheckBox->setChecked(settings.value("auto_export_svg", false).toBool());
        settings.endGroup();
    }

    void saveAutoExportConfig()
    {
        QSettings settings;
        settings.beginGroup("task_queue");
        settings.setValue("auto_export_enabled", ui->autoExportGroupBox->isChecked());
        settings.setValue("auto_export_dir", ui->autoExportDirEdit->text());
        settings.setValue("auto_export_png", ui->autoExportPngCheckBox->isChecked());
        settings.setValue("auto_export_svg", ui->autoExportSvgCheckBox->isChecked());
        settings.endGroup();
    }

    void exportTaskResults(const QPointer<ImageTaskWindow>& window)
    {
        const AutoExportConfig config{autoExportConfig()};
        if(!config.enabled || (!config.png && !config.svg)) {
            return;
        }
        if(window.isNull() || window->getImageTask() == nullptr) {
            return;
        }

        // 命名 = 源图文件名(去扩展);拖入项可能是 file:/// URL,统一取文件名部分
        const QString sourceName{windowImagePath(window)};
        const QString baseName{QFileInfo(QUrl(sourceName).toLocalFile().isEmpty() ? sourceName : QUrl(sourceName).toLocalFile()).completeBaseName()};
        const QString safeBaseName{baseName.isEmpty() ? QStringLiteral("task") : baseName};
        const int occurrence{m_exportNameCounter.value(safeBaseName, 0)};
        m_exportNameCounter[safeBaseName] = occurrence + 1;
        const QString uniqueName{occurrence == 0 ? safeBaseName : safeBaseName + "_" + QString::number(occurrence + 1)};

        QDir outputDir(config.directory);
        if(!outputDir.exists() && !outputDir.mkpath(".")) {
            qWarning() << "Failed to create batch output directory:" << config.directory;
            q->statusBar()->showMessage(tr("Failed to create output directory: %1").arg(config.directory), 5000);
            return;
        }

        const task::ImageTask* imageTask{window->getImageTask()};
        const geometrize::Bitmap& current{imageTask->getCurrent()};
        const std::uint32_t width{current.getWidth()};
        const std::uint32_t height{current.getHeight()};

        if(config.png) {
            const QString pngPath{outputDir.filePath(uniqueName + ".png")};
            if(!geometrize::exporter::exportBitmap(current, pngPath.toStdString())) {
                qWarning() << "Failed to export batch PNG:" << pngPath;
            }
        }
        if(config.svg) {
            const QString svgPath{outputDir.filePath(uniqueName + ".svg")};
            const std::vector<geometrize::ShapeResult>& shapes{window->getShapes()};
            const std::string svgData{geometrize::exporter::exportSVG(shapes, width, height)};
            if(!svgData.empty()) {
                util::writeStringToFile(svgData, svgPath.toStdString());
            } else {
                qWarning() << "Failed to export batch SVG:" << svgPath;
            }
        }
    }

    // ---- 队列持久化:每次变更即写(事件驱动,崩溃安全) ----

    void persistQueue() const
    {
        QSettings settings;
        settings.beginGroup("task_queue");
        QStringList paths;
        for(int i = 0; i < ui->taskList->count(); ++i) {
            paths.push_back(ui->taskList->item(i)->data(Qt::UserRole).toString());
        }
        settings.setValue("pending_paths", paths);
        settings.setValue("selected_script", ui->scriptSelectComboBox->currentText());
        settings.endGroup();
    }

    void restoreQueue()
    {
        QSettings settings;
        settings.beginGroup("task_queue");
        const QStringList paths{settings.value("pending_paths", QStringList()).toStringList()};
        const QString selectedScript{settings.value("selected_script").toString()};
        settings.endGroup();

        for(const QString& path : paths) {
            addItem(path, path);
        }
        if(!selectedScript.isEmpty() && m_scripts.count(selectedScript.toStdString()) > 0) {
            ui->scriptSelectComboBox->setCurrentText(selectedScript);
            // setCurrentText 与当前值相同时不触发 currentTextChanged,编辑器须显式跟随
            m_scriptEditorWidget->setCurrentCode(m_scripts.at(selectedScript.toStdString()));
        }
    }

    void populateUi()
    {
    }

    std::unique_ptr<Ui::TaskQueueWindow> ui{nullptr};
    TaskQueueWindow* q{nullptr};

    geometrize::dialog::ScriptEditorWidget* m_scriptEditorWidget;

    const std::string defaultScriptName{ "default" };
    std::map<std::string, std::string> m_scripts;

    // 批处理受管窗口集合:QPointer 随窗口销毁自动失效,轮询据此做销毁兜底。
    // QPointer 在 Qt5 无 qHash,集合一律用向量线性查找(批处理窗口数量级小,无性能问题)
    QVector<QPointer<ImageTaskWindow>> m_trackedWindows;
    QVector<QPointer<ImageTaskWindow>> m_completedWindows;
    QHash<QString, int> m_exportNameCounter;
    QVector<QPair<QPointer<ImageTaskWindow>, QString>> m_windowImagePath;

#ifdef Q_OS_WIN
    std::unique_ptr<TaskbarProgress> m_taskbar;
#endif
};

TaskQueueWindow::TaskQueueWindow() :
    QMainWindow{nullptr},
    d{std::make_unique<TaskQueueWindow::TaskQueueWindowImpl>(this)}
{
}

TaskQueueWindow::~TaskQueueWindow()
{
}

void TaskQueueWindow::on_actionExit_triggered()
{
    d->close();
}

void TaskQueueWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        d->onLanguageChange();
    }
    QMainWindow::changeEvent(event);
}

void TaskQueueWindow::dragEnterEvent(QDragEnterEvent* event)
{
    event->acceptProposedAction();
}

void TaskQueueWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls{geometrize::format::getUrls(event->mimeData())};
    QStringList tasks;
    for(const QUrl& url : urls) {
        const QString urlString{url.toString()};
        tasks.push_back(urlString);
    }
    d->addItems(tasks);
}

}

}
