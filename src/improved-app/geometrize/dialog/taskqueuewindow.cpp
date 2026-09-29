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
#include <set>
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
        // S_FALSE(本线程已按同一套间模型初始化过)时引用计数未增加,此时配对 CoUninitialize
        // 会去减别人的计数(Qt 平台插件已 OleInitialize),可能把 OLE 提前拆掉。
        // 只有本次真正初始化成功(S_OK)才由我们负责配对释放。
        m_comOwned = (coInit == S_OK);
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
            // 批处理进度归集:完成事实由信号驱动,销毁兜底由 destroyed 信号按窗口身份计数
            // (QPointer 销毁后折叠为 null,无法区分多个已销毁窗口,不能再用占位判重)。
            // 这里只负责展示与按钮状态刷新。
            const bool enableRunClearButtons = ui->taskList->count() > 0;
            ui->runTasksButton->setEnabled(enableRunClearButtons);
            ui->clearTaskListButton->setEnabled(enableRunClearButtons);
            ui->closeOpenWindowsButton->setEnabled(!geometrize::dialog::ImageTaskWindow::getExistingImageTaskWindows().empty());

            const std::uint32_t completed{static_cast<std::uint32_t>(m_completedIds.size() + m_destroyedUncompletedCount)};
            const std::uint32_t total{static_cast<std::uint32_t>(m_managedTotal)};
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
            // 销毁兜底:identity 仅作比较用(销毁后不解引用);批次重置后迟到的销毁信号不计入
            const void* id{window};
            connect(window, &QObject::destroyed, q, [this, id](QObject*) {
                onTrackedWindowDestroyed(id);
            });
            m_managedTotal++;
            m_managedIds.insert(id);
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
        if(window.isNull()) {
            return;
        }
        const void* id{window.data()};
        // 批次重置后迟到的完成信号(m_managedIds 已清)不得计入新批次:否则标题出现 "2/1" 这类
        // 计数,且导出时查不到源图路径会回落到字面量 "task" 文件名(与销毁兜底同口径)
        if(m_managedIds.find(id) == m_managedIds.end()) {
            return;
        }
        if(std::find(m_completedIds.begin(), m_completedIds.end(), id) != m_completedIds.end()) {
            return;
        }
        m_completedIds.push_back(id);
        exportTaskResults(window);
    }

    // 受管窗口销毁兜底:未发完成信号的窗口按"完成"计数(与原占位语义一致,但可分辨身份)。
    // 批次重置后迟到的销毁信号(m_managedIds 已清)不计入。
    void onTrackedWindowDestroyed(const void* id)
    {
        if(m_managedIds.erase(id) == 0) {
            return;
        }
        if(std::find(m_completedIds.begin(), m_completedIds.end(), id) == m_completedIds.end()) {
            m_destroyedUncompletedCount++;
        }
        m_trackedWindows.erase(std::remove_if(m_trackedWindows.begin(), m_trackedWindows.end(),
            [](const QPointer<ImageTaskWindow>& w) { return w.isNull(); }), m_trackedWindows.end());
        removeWindowImagePath(QPointer<ImageTaskWindow>());
    }

    void resetBatchProgress()
    {
        m_trackedWindows.clear();
        m_completedIds.clear();
        m_managedIds.clear();
        m_managedTotal = 0;
        m_destroyedUncompletedCount = 0;
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

        bool exportOk{true};
        if(config.png) {
            const QString pngPath{outputDir.filePath(uniqueName + ".png")};
            if(!geometrize::exporter::exportBitmap(current, pngPath.toStdString())) {
                qWarning() << "Failed to export batch PNG:" << pngPath;
                exportOk = false;
            }
        }
        if(config.svg) {
            const QString svgPath{outputDir.filePath(uniqueName + ".svg")};
            const std::vector<geometrize::ShapeResult>& shapes{window->getShapes()};
            const std::string svgData{geometrize::exporter::exportSVG(shapes, width, height)};
            if(!svgData.empty()) {
                if(!util::writeStringToFile(svgData, svgPath.toStdString())) {
                    qWarning() << "Failed to export batch SVG:" << svgPath;
                    exportOk = false;
                }
            } else {
                qWarning() << "Failed to export batch SVG:" << svgPath;
                exportOk = false;
            }
        }
        if(!exportOk) {
            // 此前写盘失败只进 qWarning:磁盘满/目录只读时进度条照走 N/N,用户会以为导出成功
            q->statusBar()->showMessage(tr("Failed to export some results to %1").arg(config.directory), 8000);
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

    // 批处理受管窗口集合:完成/销毁计数用窗口原始指针作身份(QPointer 销毁后折叠为
    // null,无法区分多个已销毁窗口);原始指针在销毁后仅作比较,绝不解引用。
    // m_trackedWindows 保留 QPointer 用于存活窗口判重(跨 eval 快照 diff)。
    QVector<QPointer<ImageTaskWindow>> m_trackedWindows;
    std::vector<const void*> m_completedIds;   // 已发完成信号窗口的身份(含销毁后)
    std::set<const void*> m_managedIds;        // 尚未销毁的受管窗口身份(迟到销毁信号判重)
    int m_managedTotal{0};                     // 本批受管窗口总数(单调)
    int m_destroyedUncompletedCount{0};        // 销毁时未发完成信号的窗口数(按完成计)
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
