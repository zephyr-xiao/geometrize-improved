#include "imagetaskwindow.h"
#include "ui_imagetaskwindow.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <string>
#include <vector>

#include <QByteArray>
#include <QCryptographicHash>
#include <QEvent>
#include <QKeySequence>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QStatusBar>
#include <QTimer>

#include "chaiscript/chaiscript.hpp"

#include "geometrize/bitmap/bitmap.h"
#include "geometrize/commonutil.h"
#include "geometrize/runner/imagerunneroptions.h"
#include "geometrize/shaperesult.h"

#include "common/uiactions.h"
#include "common/util.h"
#include "dialog/imagetaskimagewidget.h"
#include "dialog/scripteditorwidget.h"
#include "image/imageloader.h"
#include "localization/strings.h"
#include "preferences/globalpreferences.h"
#include "script/geometrizerengine.h"
#include "script/command.h"
#include "script/commandhandler.h"
#include "scene/imagetaskgraphicsview.h"
#include "scene/imagetaskscenemanager.h"
#include "task/imagetask.h"
#include "task/shapecollection.h"
#include "version/versioninfo.h"
#include "tabletproximityeventfilter.h"

#if defined DATASLINGER_INCLUDED
#include "dataslinger/imageslinger.h"
#include "geometrize/exporter/svgexporter.h"
#endif

namespace
{

// Utility function for destroying an image task set on a window.
// This has a special case because we may need to defer task deletion until the task is finished working.
void destroyTask(geometrize::task::ImageTask* task)
{
    if(task == nullptr) {
        assert(0 && "Attempted to destroy an image task that was already null");
        return;
    }

    if(task->isStepping()) {
        // Wait until the task finishes stepping before disposing of it
        // Otherwise it will probably crash as the Geometrize library will be working with deleted data
        task->connect(task, &geometrize::task::ImageTask::signal_modelDidStep, [task](std::vector<geometrize::ShapeResult>) {
            task->disconnect();
            task->deleteLater();
        });
        task->connect(task, &geometrize::task::ImageTask::signal_modelDidReplay, [task](std::vector<geometrize::ShapeResult>) {
            task->disconnect();
            task->deleteLater();
        });
    } else {
        task->disconnect();
        task->deleteLater();
    }
}

}

namespace geometrize
{

namespace dialog
{

class ImageTaskWindow::ImageTaskWindowImpl
{
public:
    ImageTaskWindowImpl(ImageTaskWindow* pQ) :
        ui{std::make_unique<Ui::ImageTaskWindow>()},
        q{pQ}
    {
        ui->setupUi(q);
        populateUi();
        q->setAttribute(Qt::WA_DeleteOnClose);

        // Set up the dock widgets
        q->tabifyDockWidget(ui->runnerSettingsDock, ui->scriptsDock);
        q->tabifyDockWidget(ui->runnerSettingsDock, ui->exporterDock);

        ui->runnerSettingsDock->raise(); // Make sure runner settings dock is selected

        // 撤销/重做:按钮信号与菜单 action 汇合同一实现;快捷键挂 QAction(窗口级作用域)
        connect(ui->imageTaskRunnerWidget, &ImageTaskRunnerWidget::undoButtonClicked, [this]() {
            undoModel();
        });
        connect(ui->imageTaskRunnerWidget, &ImageTaskRunnerWidget::redoButtonClicked, [this]() {
            redoModel();
        });
        connect(ui->imageTaskRunnerWidget, &ImageTaskRunnerWidget::priorityRegionSelectionModeChanged, [this](bool enabled) {
            m_regionSelectMode = enabled;
            if(!enabled) {
                m_regionDragActive = false;
                m_sceneManager.setPriorityRegionPreviewRect(QRectF()); // 隐藏预览
            }
        });
        connect(ui->imageTaskRunnerWidget, &ImageTaskRunnerWidget::clearPriorityRegionsClicked, [this]() {
            m_sceneManager.setPriorityRegions({});
            ui->imageTaskRunnerWidget->syncUserInterface();
        });
        connect(ui->actionUndo, &QAction::triggered, [this]() {
            undoModel();
        });
        connect(ui->actionRedo, &QAction::triggered, [this]() {
            redoModel();
        });
        ui->actionUndo->setShortcuts({QKeySequence::Undo}); // Ctrl+Z
        ui->actionRedo->setShortcuts({QKeySequence("Ctrl+Shift+Z"), QKeySequence("Ctrl+Y")});

        ui->consoleWidget->setVisible(false); // Make sure console widget is hidden by default

        // Set up the image task geometrization views
        m_pixmapView = new geometrize::scene::ImageTaskGraphicsView(ui->imageViewContainer);
        ui->imageViewContainer->layout()->addWidget(m_pixmapView);

        m_svgView = new geometrize::scene::ImageTaskGraphicsView(ui->imageViewContainer);
        ui->imageViewContainer->layout()->addWidget(m_svgView);

        m_sceneManager.setViews(*m_pixmapView, *m_svgView);

        // Set initial view visibility
        const geometrize::preferences::GlobalPreferences& prefs{geometrize::preferences::getGlobalPreferences()};
        setConsoleVisibility(prefs.shouldShowImageTaskConsoleByDefault());
        setPixmapViewVisibility(prefs.shouldShowImageTaskPixmapViewByDefault());
        setVectorViewVisibility(prefs.shouldShowImageTaskVectorViewByDefault());

        // Handle clicks on checkable title bar items
        connect(ui->actionScript_Console, &QAction::toggled, [this](const bool checked) {
            setConsoleVisibility(checked);
        });
        connect(ui->actionPixmap_Results_View, &QAction::toggled, [this](const bool checked) {
            setPixmapViewVisibility(checked);
        });
        connect(ui->actionVector_Results_View, &QAction::toggled, [this](const bool checked) {
            setVectorViewVisibility(checked);
        });

        // Handle request to set the image task on the task window
        connect(q, &ImageTaskWindow::willSwitchImageTask, [this](task::ImageTask* lastTask, task::ImageTask* nextTask) {
            // Disconnect the last image task, which will soon be replaced by the next image task
            disconnectTask();

            m_shapes.clear();
            // 任务切换:撤销/重做状态整体作废(旧任务的 redo 项不适用新任务)
            m_redoStack.clear();
            m_replayInFlight = false;
            m_redoInFlight = false;
            m_lastRedrawnShape = nullptr;
            m_regionDragActive = false;
            m_sceneManager.setPriorityRegionPreviewRect(QRectF());
            ui->imageTaskRunnerWidget->setImageTask(nextTask);
            ui->scriptsWidget->setImageTask(nextTask);
            ui->imageTaskExportWidget->setImageTask(nextTask, &m_shapes.getShapeVector());

            const auto taskIdForSerialization = [](const task::ImageTask& task) {
                std::string s = std::to_string(task.getWidth()) + "_" + std::to_string(task.getHeight()) + "_";

                QCryptographicHash hash(QCryptographicHash::Md5);
                hash.addData(reinterpret_cast<const char*>(task.getTarget().getDataRef().data()), task.getTarget().getDataRef().size());
                const QByteArray result = hash.result().toHex();
                const std::string hashStr = std::string(result.begin(), result.end());
                return s + hashStr;
            };

            // Autosave the preferences for the task being switched away from, if there was one
            if(lastTask != nullptr && geometrize::preferences::getGlobalPreferences().shouldAutoSaveImageTaskSettings()) {
                const std::string path{geometrize::preferences::getImageTaskPreferencesAutoSavePath(taskIdForSerialization(*lastTask))};
                if(path.empty()) {
                    assert(0 && "Auto save path for image task file must not be empty");
                }
                saveSettingsTemplate(lastTask, path);
            }

            // Autoload the preferences for the task being switched to, if there is one
            if(nextTask != nullptr && geometrize::preferences::getGlobalPreferences().shouldAutoLoadImageTaskSettings()) {
                const std::string path{geometrize::preferences::getImageTaskPreferencesAutoSavePath(taskIdForSerialization(*nextTask))};
                if(path.empty()) {
                    assert(0 && "Auto load path for image task file must not be empty");
                }
                loadSettingsTemplate(nextTask, path);
            }
        });
        connect(q, &ImageTaskWindow::didSwitchImageTask, [this](task::ImageTask* lastTask, task::ImageTask* currentTask) {
            if(currentTask == nullptr) {
                m_taskPreferencesSetConnection = QMetaObject::Connection{};
                m_taskWillStepConnection = QMetaObject::Connection{};
                m_taskDidReplayConnection = QMetaObject::Connection{};
                // 区域选择状态随任务作废
                m_regionSelectMode = false;
                m_regionDragActive = false;
                m_sceneManager.setPriorityRegions({});
                m_sceneManager.setPriorityRegionPreviewRect(QRectF());
                return; // Don't try to connect if we're setting the current task to nothing/nullptr
            }

            m_taskPreferencesSetConnection = connect(currentTask, &task::ImageTask::signal_preferencesSet, [this]() {
                ui->imageTaskRunnerWidget->syncUserInterface();
                ui->scriptsWidget->syncUserInterface();
            });

            m_taskWillStepConnection = connect(currentTask, &task::ImageTask::signal_modelWillStep, [this]() {
                ui->statsDockContents->setCurrentStatus(geometrize::dialog::ImageTaskStatsWidget::RUNNING);
                updateUndoRedoActions(); // 步进/重放开始即禁用(守卫的 isStepping 分支同时兜底)

                // Apply the latest scripts and engine state prior to stepping
                auto& geometrizer = m_task->getGeometrizer();

                m_task->getPreferences().setScripts(ui->scriptsWidget->getScripts());

                chaiscript::ChaiScript* engine = geometrizer.getEngine();
                engine->set_global(chaiscript::var(m_shapes.getShapeVector().size()), "currentShapeCount");

                ui->scriptsWidget->evaluateBeforeStepScripts();
            });

            m_taskDidStepConnection = connect(currentTask, &task::ImageTask::signal_modelDidStep, [this](std::vector<geometrize::ShapeResult> shapes) {
                processPostStepCbs();

                // 撤销重放在途:丢弃竞争步进的结果(经典编辑器语义——在途编辑被丢弃),不进 append/脚本/链式步进
                if(m_replayInFlight) {
                    updateStats();
                    updateUndoRedoActions();
                    return;
                }

                // redo 自身的 drawShape 产物:不清 redo 栈;其余任何来源的新形状都使 redo 失效
                if(m_redoInFlight && !shapes.empty() && shapes.back().shape == m_lastRedrawnShape) {
                    m_redoInFlight = false;
                } else if(!shapes.empty() && !m_redoStack.empty()) {
                    m_redoStack.clear();
                }

                // If the first shape added background rectangle then fit the scenes to it
                if(m_shapes.empty()) {
                    m_sceneManager.fitScenesInViews(*m_pixmapView, *m_svgView);
                }
                m_shapes.appendShapes(shapes);

                updateStats();

                ui->scriptsWidget->evaluateAfterStepScripts();

                if(shouldKeepStepping()) {
                    stepModel();
                }
            });

            m_taskDidReplayConnection = connect(currentTask, &task::ImageTask::signal_modelDidReplay, [this](std::vector<geometrize::ShapeResult> shapes) {
                m_replayInFlight = false;
                // 防御对账:worker 重放以列表为权威,列表若因竞争步进被丢弃不会小于此处
                if(m_shapes.size() > shapes.size()) {
                    m_shapes.truncate(shapes.size());
                }
                updateStats();
                m_pendingSceneShapes.clear();
                // 此时 worker 已空闲,读位图安全;pixmap 全量重传 + SVG 场景整体重建
                const QPixmap pixmap{image::createPixmap(m_task->getCurrent())};
                m_sceneManager.reset();
                m_sceneManager.updateScenes(pixmap, shapes);
                updateUndoRedoActions();
            });

            // 从新任务的偏好恢复区域 overlay 显示
            refreshPriorityRegionOverlays();

            const QString windowTitle = [currentTask]() {
                QString title = geometrize::strings::Strings::getApplicationName()
                        .append(" ").append(geometrize::version::getApplicationVersionString());
                if(currentTask != nullptr) {
                    title.append(" ").append(QString::fromStdString(currentTask->getDisplayName()));
                }
                return title;
            }();
            q->setWindowTitle(windowTitle);

            if(currentTask != nullptr) {
                const QPixmap target{image::createPixmap(m_task->getTarget())};
                m_sceneManager.setTargetPixmap(target);
            } else {
                m_sceneManager.setTargetPixmap(QPixmap());
            }

            if(currentTask != nullptr) {
                const auto getStartingColor = [this]() {
                    const geometrize::preferences::GlobalPreferences& prefs{geometrize::preferences::getGlobalPreferences()};
                    if(prefs.shouldUseCustomImageTaskBackgroundOverrideColor()) {
                        const auto color = prefs.getCustomImageTaskBackgroundOverrideColor();
                        return geometrize::rgba{ static_cast<std::uint8_t>(color[0]), static_cast<std::uint8_t>(color[1]), static_cast<std::uint8_t>(color[2]), static_cast<std::uint8_t>(color[3]) };
                    }
                    return geometrize::commonutil::getAverageImageColor(m_task->getTarget());
                };
                currentTask->drawBackgroundRectangle(getStartingColor());
            }

            if(currentTask != nullptr) {
                ui->consoleWidget->setEngine(currentTask->getGeometrizer().getEngine());
            } else {
                ui->consoleWidget->setEngine(nullptr);
            }

            ui->imageTaskRunnerWidget->syncUserInterface();
            ui->scriptsWidget->syncUserInterface();

            if(currentTask != nullptr) {
                ui->imageTaskImageWidget->setTargetImage(image::createImage(currentTask->getTarget()));
            } else {
                ui->imageTaskImageWidget->setTargetImage(QImage());
            }

            m_timeRunning = 0.0f;

            #if defined DATASLINGER_INCLUDED
            // Bind keyboard shortcuts, setup UI for sending images out over the network etc
            geometrize::installImageSlingerUserInterface(q);
            #endif

            // As a final step, dispose of the old image task, if there was one
            if(lastTask != nullptr) {
                destroyTask(lastTask);
            }
        });

        // Handle requested target image overlay opacity changes
        connect(ui->imageTaskImageWidget, &ImageTaskImageWidget::targetImageOpacityChanged, [this](const unsigned int value) {
            const float opacity{value * (1.0f / 255.0f)};
            m_sceneManager.setTargetPixmapOpacity(opacity);
        });

        // Handle a request to change the target image
        connect(ui->imageTaskImageWidget, &ImageTaskImageWidget::targetImageSelected, [this](const QImage& image) {
            assert(!image.isNull());

            // Validate the target image size
            const geometrize::Bitmap& target{m_task->getTarget()};
            const int targetWidth{static_cast<int>(target.getWidth())};
            const int targetHeight{static_cast<int>(target.getHeight())};
            if(targetWidth != image.width() || targetHeight != image.height()) {
                const QString selectedImageSize(tr("%1x%2", "Dimensions of an image e.g. width-x-height, 1024x800").arg(QLocale().toString(image.width())).arg(QLocale().toString(image.height())));
                const QString targetImageSize(tr("%1x%2", "Dimensions of an image e.g. width-x-height, 1024x800").arg(QLocale().toString(targetWidth)).arg(QLocale().toString(targetHeight)));
                QMessageBox::warning(
                            q,
                            tr("Image has incorrect dimensions", "Title of an error dialog shown when the user selects an image that was the wrong resolution/size"),
                            tr("Selected image must have the same dimensions as the current target image. Size was %1, but should have been %2",
                               "Error message shown when the user selects an image that was the wrong resolution/size").arg(selectedImageSize).arg(targetImageSize));

                return;
            }

            setTargetImage(image);
        });

        // Handle a request to change the target image that has passed size checks and validation
        connect(ui->imageTaskImageWidget, &ImageTaskImageWidget::targetImageSet, [this](const QImage& image) {
            // If the task is running then defer the target image change to the next step, else do it immediately
            if(isRunning()) {
                const QImage imageCopy{image.copy()};
                addPostStepCb([this, imageCopy]() {
                    Bitmap target = geometrize::image::createBitmap(imageCopy);
                    switchTargetImage(target);
                });
            } else {
                Bitmap target = geometrize::image::createBitmap(image);
                switchTargetImage(target);
            }
        });

        // Handle runner button presses
        connect(ui->imageTaskRunnerWidget, &ImageTaskRunnerWidget::runStopButtonClicked, [this]() {
            setShouldKeepStepping(!shouldKeepStepping());

            // Request another image task step if user clicked start here
            if(shouldKeepStepping()) {
                stepModel();
            }
        });
        connect(ui->imageTaskRunnerWidget, &ImageTaskRunnerWidget::stepButtonClicked, [this]() {
            stepModel();
        });
        connect(ui->imageTaskRunnerWidget, &ImageTaskRunnerWidget::clearButtonClicked, [this]() {
            clearModel();
        });

        connect(q, &ImageTaskWindow::didLoadSettingsTemplate, [this]() {
            ui->imageTaskRunnerWidget->syncUserInterface();
            ui->scriptsWidget->syncUserInterface();
        });

        // Track how long the task has been in the running state
        connect(&m_timeRunningTimer, &QTimer::timeout, [this]() {
            if(isRunning()) {
                m_timeRunning += m_timeRunningTimerResolutionMs;
                updateStats();
            }
        });

        connect(&m_scriptEngineUpdateTimer, &QTimer::timeout, [this]() {
            ui->scriptsWidget->evaluateOnTimedUpdateEventScripts();
        });

        // 33ms 合帧定时器:单发 + 以窗口为接收者(窗口销毁自动断连);Impl 析构时显式停止,
        // 避免回调在 Impl 已析构后触发(旧写法用 QPointer 守护窗口,但 Impl 早于窗口的 ~QObject 析构)
        m_sceneUpdateTimer.setSingleShot(true);
        m_sceneUpdateTimer.setInterval(33);
        connect(&m_sceneUpdateTimer, &QTimer::timeout, q, [this]() { flushPendingSceneUpdate(); });

        // 脚本增删后重新判定 timed-update 轮询是否需要运行(构造期脚本尚未装载,不能只在那里判定)
        connect(ui->scriptsWidget, &geometrize::dialog::ImageTaskScriptingWidget::signal_scriptChanged, q,
                [this](const std::string&, const std::string&) { syncScriptUpdateTimer(); });

        // Before shapes are added, evaluate he pre-add shape callbacks
        connect(&m_shapes, &geometrize::task::ShapeCollection::signal_beforeAppendShapes, [this](const std::vector<geometrize::ShapeResult>&) {
            ui->scriptsWidget->evaluateBeforeAddShapeScripts();
        });

        // After shapes are added, evaluate the post-add shape callbacks and stop conditions
        connect(&m_shapes, &geometrize::task::ShapeCollection::signal_afterAppendShapes, [this](const std::vector<geometrize::ShapeResult>&) {
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            engine->set_global(chaiscript::var(m_shapes.back().shape->clone()), "lastAddedShape");
            engine->set_global(chaiscript::var(m_shapes.back().score), "lastAddedShapeScore");
            engine->set_global(chaiscript::var(m_shapes.back().color), "lastAddedShapeColor");

            ui->scriptsWidget->evaluateAfterAddShapeScripts();

            if(!ui->scriptsWidget->evaluateStopConditionScripts()) {
                return;
            }
            setShouldKeepStepping(false);
            ui->scriptsWidget->evaluateOnStopConditionMetScripts();
            //geometrize::dialog::showImageTaskStopConditionMetMessage(q); // Letting the scripts handle notifying the user
            // 任务自然到达停止条件后通知外界(批处理宿主以此感知完成并触发自动导出);
            // 此刻在主线程(worker didStep 走 BlockingQueuedConnection),读任务位图/形状列表安全
            emit q->signal_didStopConditionMet();
        });

        // Update the graphical image views after shapes are added.
        // 节流合帧:高速步进时每步一次全图 Bitmap→QPixmap 上传代价高,改为 33ms 合并窗口,
        // 到期时按"当时"的画面渲染一次,视觉上与逐步刷新无差。
        connect(&m_shapes, &geometrize::task::ShapeCollection::signal_afterAppendShapes, [this](const std::vector<geometrize::ShapeResult>& shapes) {
            // ShapeResult 成员为 const(不可赋值),只能追加构造:重建整个待渲染列表
            m_pendingSceneShapes.reserve(m_pendingSceneShapes.size() + shapes.size());
            for(const auto& shape : shapes) {
                m_pendingSceneShapes.emplace_back(shape);
            }
            if(!m_sceneUpdatePending) {
                m_sceneUpdatePending = true;
                // 本回调运行在 didStep 栅栏内(worker 阻塞在信号投递上),此刻读 m_current 安全。
                // 快照只在此处取一次;33ms 后的渲染回调不再触碰 worker 的活位图。
                if(m_task != nullptr && !m_replayInFlight) {
                    m_pendingBitmap = m_task->getCurrent();
                }
                m_sceneUpdateTimer.start();
            }
        });

        #if defined DATASLINGER_INCLUDED
        // Send the newly added SVG shape data out to listening clients
        connect(&m_shapes, &geometrize::task::ShapeCollection::signal_afterAppendShapes, [](const std::vector<geometrize::ShapeResult>& shapes) {

            // Exporting rotated ellipses as polygons, since OpenFL's SVG library can't handle regular rotated SVG ellipse or paths
            geometrize::exporter::SVGExportOptions options;
            options.rotatedEllipseExportMode = geometrize::exporter::RotatedEllipseSVGExportMode::POLYGON;

            for(const auto& result : shapes) {
                geometrize::sendSvgShapeData(geometrize::exporter::getSingleShapeSVGData(result.color, *result.shape, options));
            }
        });
        #endif

        // Update the graphical image views when the number of shapes changes (e.g. when cleared)
        connect(&m_shapes, &geometrize::task::ShapeCollection::signal_sizeChanged, [this](const std::size_t size) {
            if(size == 0) {
                // 清空立即刷新并丢弃待合并帧,防残影
                m_pendingSceneShapes.clear();
                m_sceneManager.reset();
            }
        });

        // Pass the latest mouse event info to the current image task's script engine
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageHoverMoveEvent, [this](double lastX, double lastY, double x, double y, bool /*ctrlModifier*/) {
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            engine->set_global(chaiscript::var(lastX), "targetImageLastMouseX");
            engine->set_global(chaiscript::var(lastY), "targetImageLastMouseY");
            engine->set_global(chaiscript::var(x), "targetImageMouseX");
            engine->set_global(chaiscript::var(y), "targetImageMouseY");

            ui->scriptsWidget->evaluateOnMouseMoveEventScripts();
        });
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageMousePressEvent, [this](double x, double y, bool ctrlModifier) {
            // 区域选择模式:Ctrl+按下开始框选,不喂脚本引擎(旧行为在模式关闭时逐位保留)
            if(m_regionSelectMode && ctrlModifier && m_task != nullptr && !m_task->isStepping()) {
                m_regionDragStart = QPointF(x, y);
                m_regionDragActive = true;
                return;
            }
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            engine->set_global(chaiscript::var(x), "targetImageLastMouseX");
            engine->set_global(chaiscript::var(y), "targetImageLastMouseY");
            engine->set_global(chaiscript::var(x), "targetImageMouseX");
            engine->set_global(chaiscript::var(y), "targetImageMouseY");

            ui->scriptsWidget->evaluateOnMouseDownEventScripts();
        });
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageMouseMoveEvent, [this](double lastX, double lastY, double x, double y, bool /*ctrlModifier*/) {
            // 区域框选拖拽中:更新预览矩形,不喂脚本引擎
            if(m_regionDragActive) {
                m_sceneManager.setPriorityRegionPreviewRect(QRectF(m_regionDragStart, QPointF(x, y)).normalized());
                return;
            }
            // NOTE not triggered currently, hover move events are used instead
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            engine->set_global(chaiscript::var(lastX), "targetImageLastMouseX");
            engine->set_global(chaiscript::var(lastY), "targetImageLastMouseY");
            engine->set_global(chaiscript::var(x), "targetImageMouseX");
            engine->set_global(chaiscript::var(y), "targetImageMouseY");

            ui->scriptsWidget->evaluateOnMouseMoveEventScripts();
        });
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageMouseReleaseEvent, [this](double x, double y, bool /*ctrlModifier*/) {
            // 区域框选结束:有效矩形(宽高 > 2 场景px)且未超上限(16)时提交为百分比区域
            if(m_regionDragActive) {
                m_regionDragActive = false;
                m_sceneManager.setPriorityRegionPreviewRect(QRectF()); // 隐藏预览
                const QRectF rect{QRectF(m_regionDragStart, QPointF(x, y)).normalized()};
                const auto task = m_task;
                if(task != nullptr && rect.width() > 2.0 && rect.height() > 2.0) {
                    auto regions{task->getPreferences().getImageRunnerOptions().priorityRegions};
                    if(regions.size() < 16) {
                        const std::uint32_t w{task->getWidth()};
                        const std::uint32_t h{task->getHeight()};
                        // 场景像素 → 百分比(与 mapShapeBoundsToImage 同款分母 dim-1)
                        regions.push_back(geometrize::ImageRunnerPriorityRegionOptions{
                            100.0 * rect.left() / static_cast<double>(w - 1),
                            100.0 * rect.top() / static_cast<double>(h - 1),
                            100.0 * rect.right() / static_cast<double>(w - 1),
                            100.0 * rect.bottom() / static_cast<double>(h - 1)});
                        task->getPreferences().setPriorityRegions(regions);
                        refreshPriorityRegionOverlays();
                        ui->imageTaskRunnerWidget->syncUserInterface();
                    } else {
                        // 上限静默丢弃会被当成"框选坏了":至少给一条状态栏提示
                        q->statusBar()->showMessage(tr("Priority region limit (16) reached — clear regions to add more"), 5000);
                    }
                }
                return;
            }
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            engine->set_global(chaiscript::var(x), "targetImageLastMouseX");
            engine->set_global(chaiscript::var(y), "targetImageLastMouseY");
            engine->set_global(chaiscript::var(x), "targetImageMouseX");
            engine->set_global(chaiscript::var(y), "targetImageMouseY");

            ui->scriptsWidget->evaluateOnMouseUpEventScripts();
        });
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageWheelEvent, [this](double x, double y, int amount, bool /*ctrlModifier*/) {
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            engine->set_global(chaiscript::var(x), "targetImageLastMouseX");
            engine->set_global(chaiscript::var(y), "targetImageLastMouseY");
            engine->set_global(chaiscript::var(x), "targetImageMouseX");
            engine->set_global(chaiscript::var(y), "targetImageMouseY");
            engine->set_global(chaiscript::var(amount), "targetImageWheelMoveAmount");

            ui->scriptsWidget->evaluateOnMouseWheelEventScripts();
        });

        // Pass the latest key event info to the current image task's script engine
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageKeyPressEvent, [this](int key, bool ctrlModifier) {
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            const std::string keyString = QKeySequence(key).toString().toStdString();

            // Update vars for pressed keys in the engine (cleared on key release)
            engine->set_global(chaiscript::var(keyString), "targetImageLastKeyDown");
            engine->set_global(chaiscript::var(true), "targetImageKeyDown_" + keyString);
            engine->set_global(chaiscript::var(ctrlModifier), "targetImageControlModifierDown");

            // Last pressed key in the engine, not cleared on key release
            engine->set_global(chaiscript::var(keyString), "targetImageLastKeyDownPersistent");

            ui->scriptsWidget->evaluateOnKeyDownEventScripts();
        });
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageKeyReleaseEvent, [this](int key, bool ctrlModifier) {
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            const std::string keyString = QKeySequence(key).toString().toStdString();

            // Update vars for pressed keys in the engine
            engine->set_global(chaiscript::var(""), "targetImageLastKeyDown");
            engine->set_global(chaiscript::var(false), "targetImageKeyDown_" + keyString);
            engine->set_global(chaiscript::var(ctrlModifier), "targetImageControlModifierDown");

            ui->scriptsWidget->evaluateOnKeyUpEventScripts();
        });

        // Pass the latest tablet event info to the current image task's script engine
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onTargetImageTabletEvent, [this](const geometrize::scene::CustomTabletEvent& event) {
            if(!m_task) {
                return;
            }

            const geometrize::scene::TabletEventData data = event.getData();
            chaiscript::ChaiScript* engine = m_task->getGeometrizer().getEngine();
            engine->set_global(chaiscript::var(data), "lastTabletEvent");

            ui->scriptsWidget->evaluateOnPenInputEventScripts();

            try {
                std::shared_ptr<geometrize::Shape> aoiShape = engine->eval<std::shared_ptr<geometrize::Shape>>("aoi");
                engine->set_global(chaiscript::var(aoiShape->clone()), "aoi"); // Work around fail to assign shape to shape in the script
                m_sceneManager.setAreaOfInfluenceShape(*aoiShape);
            } catch(...) {
                // The pen input scripts should setup an area of influence shape
            }
        });

        // Connect the global pen input proximity event filter signals
        // Note these are separate from the other tablet events, since they are application-wide
        // and proximity events don't seem to include useful data e.g. position etc
        auto& sharedTabletProximityEventFilter = geometrize::getSharedTabletProximityEventFilterInstance();
        connect(&sharedTabletProximityEventFilter, &geometrize::TabletProximityEventFilter::signal_onTabletEnterProximity, q, [this]() {
            ui->scriptsWidget->evaluateOnPenProximityEnterEventScripts();
        });
        connect(&sharedTabletProximityEventFilter, &geometrize::TabletProximityEventFilter::signal_onTabletLeaveProximity, q, [this]() {
            ui->scriptsWidget->evaluateOnPenProximityExitEventScripts();
        });

        /*
        // TODO do stuff with the area of influence shape, or tell the script engine, when input happens
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onAreaOfInfluenceShapeHoverMoveEvent, [this](const int lastX, const int lastY, const int x, const int y, const bool ctrlModifier) {
            if(!ctrlModifier) {
                return;
            }
            //translateShape(x - lastX, y - lastY);
        });
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onAreaOfInfluenceShapeMouseWheelEvent, [this](const int, const int, const double amount, const bool ctrlModifier) {
            if(!ctrlModifier) {
                return;
            }
            //scaleShape(amount > 0 ? 1.03f : 0.97f);
        });
        connect(&m_sceneManager, &geometrize::scene::ImageTaskSceneManager::signal_onAreaOfInfluenceShapeKeyPressEvent, [this](const int key, const bool) {
            if(key == Qt::Key_R) { // Rotate
                //rotateShape(3);
            }
            if(key == Qt::Key_Q) { // Scale down
                //scaleShape(0.97f);
            }
            if(key == Qt::Key_A) { // Scale up
                //scaleShape(1.03f);
            }
        });
        */

        // Set initial target image opacity
        const float initialTargetImageOpacity{0};
        ui->imageTaskImageWidget->setTargetImageOpacity(static_cast<unsigned int>(initialTargetImageOpacity));

        // Start the timer used to track how long the image task has been in the running state
        m_timeRunningTimer.start(static_cast<int>(m_timeRunningTimerResolutionMs));

        // Start the timer used to regularly update the associated image task's script engine (if any).
        // 没有任何 timed-update 脚本时不启动轮询,避免空闲时 100ms 空转求值。
        // 注意:构造期脚本尚未装载(setImageTask 在构造之后调用),这里判定必然为假,
        // 因此 setImageTask/脚本变更后必须重新判定——否则 on_timed_update 脚本永久失效。
        if(ui->scriptsWidget->hasTimedUpdateScripts()) {
            m_scriptEngineUpdateTimer.start(static_cast<int>(m_scriptEngineUpdateTimerResolution));
        }
    }
    /// timed-update 轮询定时器按需启停:脚本装载/变更后都要重新判定(构造期脚本尚未装载,判定必为假)
    void syncScriptUpdateTimer()
    {
        if(ui->scriptsWidget->hasTimedUpdateScripts()) {
            if(!m_scriptEngineUpdateTimer.isActive()) {
                m_scriptEngineUpdateTimer.start(static_cast<int>(m_scriptEngineUpdateTimerResolution));
            }
        } else if(m_scriptEngineUpdateTimer.isActive()) {
            m_scriptEngineUpdateTimer.stop();
        }
    }

    /// 合帧到期后的场景刷新:只渲染 didStep 栅栏内取的位图快照,不读 worker 的活位图
    void flushPendingSceneUpdate()
    {
        m_sceneUpdatePending = false;
        if(m_task == nullptr || m_replayInFlight) {
            // 重放在途:位图正被 worker 重写,didReplay 会做权威刷新
            m_pendingSceneShapes.clear();
            return;
        }
        if(m_pendingBitmap.getWidth() == 0U || m_pendingBitmap.getHeight() == 0U) {
            return; // 尚无快照(如刚切换任务):等下一次 append 在栅栏内取快照后再排定
        }
        const QPixmap pixmap{image::createPixmap(m_pendingBitmap)};
        m_sceneManager.updateScenes(pixmap, m_pendingSceneShapes);
        m_pendingSceneShapes.clear();
    }

    ImageTaskWindowImpl& operator=(const ImageTaskWindowImpl&) = delete;
    ImageTaskWindowImpl(const ImageTaskWindowImpl&) = delete;
    ~ImageTaskWindowImpl()
    {
        // 先停合帧定时器:回调持有 this(Impl),Impl 析构后不得再触发
        m_sceneUpdateTimer.stop();
        // Sets the task in the UI etc to nothing (potentially autosaving stuff etc), then dispose of the task
        task::ImageTask* lastTask = getImageTask();
        setImageTask(nullptr);

        if(lastTask != nullptr) {
            destroyTask(lastTask);
        }
    }

    static std::vector<ImageTaskWindow*> getExistingImageTaskWindows()
    {
        std::vector<ImageTaskWindow*> windows;
        const QWidgetList allWidgets = QApplication::allWidgets();
        for (QWidget* widget : allWidgets) {
            if(ImageTaskWindow* w = dynamic_cast<ImageTaskWindow*>(widget)) {
                windows.push_back(w);
            }
        }
        return windows;
    }

    void close()
    {
        q->close();
    }

    task::ImageTask* getImageTask()
    {
        return m_task;
    }

    const std::vector<geometrize::ShapeResult>& getShapes() const
    {
        return m_shapes.getShapeVector();
    }

    void setImageTask(task::ImageTask* task)
    {
        task::ImageTask* lastTask{m_task};
        task::ImageTask* nextTask{task};
        q->willSwitchImageTask(lastTask, nextTask);
        m_task = nextTask;
        q->didSwitchImageTask(lastTask, nextTask);
        // 任务切换后脚本集合随之变化(构造期判定为假):重新判定轮询是否需要运行
        syncScriptUpdateTimer();
    }

    void revealLaunchWindow()
    {
        if(common::ui::isLaunchWindowOpen()) {
            common::ui::bringLaunchWindowToFront();
        } else {
            common::ui::openLaunchWindow();
        }
    }

    void setConsoleVisibility(const bool visible)
    {
        if(ui->actionScript_Console->isChecked() != visible) {
            ui->actionScript_Console->setChecked(visible);
        }
        ui->consoleWidget->setVisible(visible);
    }

    void setPixmapViewVisibility(const bool visible)
    {
        if(ui->actionPixmap_Results_View->isChecked() != visible) {
            ui->actionPixmap_Results_View->setChecked(visible);
        }

        m_pixmapView->setVisible(visible);
        if(visible) {
            m_sceneManager.fitPixmapSceneInView(*m_pixmapView);
        }
    }

    void setVectorViewVisibility(const bool visible)
    {
        if(ui->actionVector_Results_View->isChecked() != visible) {
            ui->actionVector_Results_View->setChecked(visible);
        }

        m_svgView->setVisible(visible);
        if(visible) {
            m_sceneManager.fitVectorSceneInView(*m_svgView);
        }
    }

    void loadSettingsTemplate()
    {
        const QString path{common::ui::openLoadImageTaskSettingsDialog(q)};
        if(path.isEmpty()) {
            return;
        }
        loadSettingsTemplate(m_task, path.toStdString());
    }

    void loadSettingsTemplate(task::ImageTask* task, const std::string& path)
    {
        if(task == nullptr) {
            return; // Can't load settings for a non-existent task
        }
        if(path.empty()) {
            assert(0 && "Auto load path for image task file must not be empty");
        }
        task->getPreferences().load(path);
        emit q->didLoadSettingsTemplate();
    }

    void saveSettingsTemplate() const
    {
        const QString path{common::ui::openSaveImageTaskSettingsDialog(q)};
        if(path.isEmpty()) {
            return;
        }
        saveSettingsTemplate(m_task, path.toStdString());
    }

    void saveSettingsTemplate(task::ImageTask* task, const std::string& path) const
    {
        if(task == nullptr) {
            return; // Can't save settings for a non-existent task
        }
        if(path.empty()) {
            assert(0 && "Auto save path for image task file must not be empty");
        }
        task->getPreferences().save(path);
        emit q->didSaveSettingsTemplate();
    }

    void onLanguageChange()
    {
        ui->retranslateUi(q);
        populateUi();
    }

    void handleCommand(const geometrize::script::Command& command)
    {
        const QString s = QString::fromStdString(command.getCommand());

        if(s == "start") {
            setShouldKeepStepping(true);
            stepModel();
        }
        if(s == "stop") {
            setShouldKeepStepping(false);
        }

        if(s == "click_start_stop_button") {
            ui->imageTaskRunnerWidget->runStopButtonClicked();
        }
        if(s == "click_step_button") {
            ui->imageTaskRunnerWidget->stepButtonClicked();
        }
        if(s == "click_clear_button") {
            ui->imageTaskRunnerWidget->clearButtonClicked();
        }
        if(s == "undo") {
            undoModel();
        }
        if(s == "redo") {
            redoModel();
        }

        if(s == "toggle_script_console") {
            ui->actionScript_Console->toggle();
        }
        if(s == "show_script_console") {
            ui->actionScript_Console->setChecked(true);
        }
        if(s == "hide_script_console") {
            ui->actionScript_Console->setChecked(false);
        }

        if(s == "toggle_pixmap_view") {
            ui->actionPixmap_Results_View->toggle();
        }
        if(s == "show_pixmap_view") {
            ui->actionPixmap_Results_View->setChecked(true);
        }
        if(s == "hide_pixmap_view") {
            ui->actionPixmap_Results_View->setChecked(false);
        }

        if(s == "toggle_vector_view") {
            ui->actionVector_Results_View->toggle();
        }
        if(s == "show_vector_view") {
            ui->actionVector_Results_View->setChecked(true);
        }
        if(s == "hide_vector_view") {
            ui->actionVector_Results_View->setChecked(false);
        }
    }

    void setCommandHandlerName(const std::string& name)
    {
        q->setObjectName(QString::fromStdString(name));
    }

    std::string getCommandHandlerName() const
    {
        return q->objectName().toStdString();
    }

private:
    void populateUi()
    {
        updateStartStopButtonText();
        updateUndoRedoActions();
        q->setWindowTitle(geometrize::strings::Strings::getApplicationName());
    }

    bool isRunning() const
    {
        if(!m_task) {
            return false;
        }
        return m_task->isStepping();
    }

    bool shouldKeepStepping() const
    {
        return m_shouldKeepStepping;
    }

    void setShouldKeepStepping(const bool stepping)
    {
        m_shouldKeepStepping = stepping;
        updateStartStopButtonText();
        updateUndoRedoActions();
    }

    void updateStartStopButtonText()
    {
        if(!shouldKeepStepping()) {
            ui->imageTaskRunnerWidget->setRunStopButtonText(tr("Start", "Text on a button that the user presses to make the app start/begin transforming an image into shapes"));
        } else {
            ui->imageTaskRunnerWidget->setRunStopButtonText(tr("Stop", "Text on a button that the user presses to make the app stop/pause transforming an image into shapes"));
        }
    }

    void stepModel()
    {
        m_task->stepModel();
    }

    void clearModel()
    {
        auto task = new geometrize::task::ImageTask(m_task->getDisplayName(), m_task->getTargetMutable());
        task->setPreferences(m_task->getPreferences());
        setImageTask(task);
    }

    // 撤销下限 = 1:index 0 恒为背景矩形(afterAppendShapes 处理器无条件 back(),空列表是既有 UB 点)
    bool canUndo() const
    {
        return m_task != nullptr && !m_task->isStepping() && !shouldKeepStepping()
            && !m_replayInFlight && !m_redoInFlight && m_shapes.size() > 1;
    }

    bool canRedo() const
    {
        return m_task != nullptr && !m_task->isStepping() && !shouldKeepStepping()
            && !m_replayInFlight && !m_redoInFlight && !m_redoStack.empty();
    }

    void undoModel()
    {
        if(!canUndo()) {
            return;
        }
        // ShapeResult 含 shared_ptr,拷贝共享所有权零位图拷贝
        m_redoStack.push_back(m_shapes.back());
        m_shapes.truncate(m_shapes.size() - 1);
        m_replayInFlight = true;
        m_pendingSceneShapes.clear(); // 丢弃在途合帧残影,didReplay 权威刷新
        updateStats();
        updateUndoRedoActions();
        m_task->replayShapes(m_shapes.getShapeVector(), getTaskStartingColor());
    }

    void redoModel()
    {
        if(!canRedo()) {
            return;
        }
        // 走正常 drawShape 全协议:didStep 处理器完成 append/统计/合帧刷新,零新增刷新代码
        m_redoInFlight = true;
        m_lastRedrawnShape = m_redoStack.back().shape;
        const geometrize::ShapeResult result = m_redoStack.back();
        m_redoStack.pop_back();
        m_task->drawShape(result.shape, result.color, result.segments);
    }

    geometrize::rgba getTaskStartingColor() const
    {
        const geometrize::preferences::GlobalPreferences& prefs{geometrize::preferences::getGlobalPreferences()};
        if(prefs.shouldUseCustomImageTaskBackgroundOverrideColor()) {
            const auto color = prefs.getCustomImageTaskBackgroundOverrideColor();
            return geometrize::rgba{ static_cast<std::uint8_t>(color[0]), static_cast<std::uint8_t>(color[1]), static_cast<std::uint8_t>(color[2]), static_cast<std::uint8_t>(color[3]) };
        }
        return geometrize::commonutil::getAverageImageColor(m_task->getTarget());
    }

    void updateUndoRedoActions()
    {
        ui->actionUndo->setEnabled(canUndo());
        ui->actionRedo->setEnabled(canRedo());
        ui->imageTaskRunnerWidget->setUndoRedoButtonsEnabled(canUndo(), canRedo());
    }

    // 把当前任务的优先区域换算回场景像素坐标显示(pixmap/svg 双场景)
    void refreshPriorityRegionOverlays()
    {
        std::vector<QRectF> rects;
        if(m_task != nullptr) {
            const std::uint32_t w{m_task->getWidth()};
            const std::uint32_t h{m_task->getHeight()};
            for(const auto& region : m_task->getPreferences().getImageRunnerOptions().priorityRegions) {
                const double x1{region.xMinPercent / 100.0 * static_cast<double>(w - 1)};
                const double y1{region.yMinPercent / 100.0 * static_cast<double>(h - 1)};
                const double x2{region.xMaxPercent / 100.0 * static_cast<double>(w - 1)};
                const double y2{region.yMaxPercent / 100.0 * static_cast<double>(h - 1)};
                rects.push_back(QRectF(QPointF((std::min)(x1, x2), (std::min)(y1, y2)), QPointF((std::max)(x1, x2), (std::max)(y1, y2))));
            }
        }
        m_sceneManager.setPriorityRegions(rects);
    }

    void switchTargetImage(Bitmap& bitmap)
    {
        Bitmap& targetBitmap{m_task->getTargetMutable()};
        targetBitmap = bitmap;
    }

    void switchCurrentImage(Bitmap& bitmap)
    {
        Bitmap& currentBitmap{m_task->getCurrentMutable()};
        currentBitmap = bitmap;
    }

    void updateStats()
    {
        ui->statsDockContents->setTaskId(m_task->getTaskId());
        ui->statsDockContents->setImageDimensions(m_task->getWidth(), m_task->getHeight());

        // Note this is "stopped" when !isRunning alone, but this prevents flashing while
        // the user has it continually adding shapes
        if(!isRunning() && !shouldKeepStepping()) {
            ui->statsDockContents->setCurrentStatus(geometrize::dialog::ImageTaskStatsWidget::STOPPED);
        }

        ui->statsDockContents->setShapeCount(m_shapes.size());

        if(!m_shapes.empty()) {
            ui->statsDockContents->setSimilarity(m_shapes.back().score * 100.0f);
        }

        ui->statsDockContents->setTimeRunning(static_cast<int>(m_timeRunning / 1000.0f));
    }

    void disconnectTask()
    {
        if(m_taskPreferencesSetConnection) {
            disconnect(m_taskPreferencesSetConnection);
        }
        if(m_taskWillStepConnection) {
            disconnect(m_taskWillStepConnection);
        }
        if(m_taskDidStepConnection) {
            disconnect(m_taskDidStepConnection);
        }
        if(m_taskDidReplayConnection) {
            disconnect(m_taskDidReplayConnection);
        }
    }

    void setTargetImage(const QImage& image)
    {
        ui->imageTaskImageWidget->setTargetImage(image);
    }

    void addPostStepCb(const std::function<void()>& f)
    {
        m_onPostStepCbs.push_back(f);
    }

    void clearPostStepCbs()
    {
        m_onPostStepCbs.clear();
    }

    void processPostStepCbs()
    {
        for(const auto& f : m_onPostStepCbs) {
            f();
        }
        clearPostStepCbs();
    }

    std::unique_ptr<Ui::ImageTaskWindow> ui{nullptr};
    ImageTaskWindow* q{nullptr};

    task::ImageTask* m_task{nullptr}; ///> The image task currently set and manipulated via this window
    QMetaObject::Connection m_taskPreferencesSetConnection{}; ///> Connection for telling the dialog when the image task preferences are set
    QMetaObject::Connection m_taskWillStepConnection{}; ///> Connection for the window to do work just prior the image task starts a step
    QMetaObject::Connection m_taskDidStepConnection{}; ///> Connection for the window to do work just after the image task finishes a step
    QMetaObject::Connection m_taskDidReplayConnection{}; ///> Connection for the window to refresh after an undo replay finishes
    std::vector<std::function<void()>> m_onPostStepCbs; ///> One-shot callbacks triggered when the image task finishes a step

    // 撤销/重做状态:redo 栈存被撤销的形状(经典双栈语义);重放期间丢弃竞争步进与新渲染帧
    std::vector<geometrize::ShapeResult> m_redoStack; ///> Shapes undone and available for redo (back = most recently undone)
    std::shared_ptr<geometrize::Shape> m_lastRedrawnShape; ///> Identity of the shape a pending redo drawShape is drawing
    bool m_replayInFlight{false}; ///> An undo replay has been dispatched and has not finished yet
    bool m_redoInFlight{false}; ///> A redo drawShape has been dispatched and its didStep has not returned yet

    // 区域优先(F3.2):Ctrl+拖拽框选状态机;模式勾选是会话操作态,不持久化
    bool m_regionSelectMode{false}; ///> Whether Ctrl+Drag is currently routed to region selection
    bool m_regionDragActive{false}; ///> A region drag is in progress
    QPointF m_regionDragStart; ///> Scene-space start point of the active drag

    geometrize::task::ShapeCollection m_shapes; ///> Collection of shapes added so far

    geometrize::scene::ImageTaskSceneManager m_sceneManager; ///> Manager for scenes containing the pixmap/vector-based representations of the shapes etc
    geometrize::scene::ImageTaskGraphicsView* m_pixmapView{nullptr}; ///> The view that holds the raster/pixel-based scene
    geometrize::scene::ImageTaskGraphicsView* m_svgView{nullptr}; ///> The view that holds the vector-based scene

    bool m_shouldKeepStepping{false}; ///> Whether to continually step i.e. whether to start another step after stepping once
    QTimer m_timeRunningTimer; ///> Timer used to keep track of how long the image task has been in the "running" state
    float m_timeRunning{0.0f}; ///> Total time that the image task has been in the "running" state
    const float m_timeRunningTimerResolutionMs{100.0f}; ///> Resolution of the time running timer in milliseconds

    QTimer m_scriptEngineUpdateTimer; ///> Timer used to call an update function on the script engine of the associated image task
    float m_scriptEngineUpdateTimerResolution{100.0f}; ///> Resolution of the script update timer in milliseconds

    // 场景刷新节流:33ms 合帧期内累积的新形状,到期一次性渲染(见 signal_afterAppendShapes 连接处)
    bool m_sceneUpdatePending{false};
    std::vector<geometrize::ShapeResult> m_pendingSceneShapes;
    // 位图快照:合帧回调不读 worker 的活位图——worker 步进时会就地改写同一缓冲(落画/回滚),
    // 主线程此刻读它是数据竞争(实测撕裂帧)。改为在 didStep 栅栏内取一份快照,回调只渲染快照。
    geometrize::Bitmap m_pendingBitmap;
    QTimer m_sceneUpdateTimer; ///> 33ms 合帧定时器(单发);Impl 析构时显式停止,避免回调触碰已析构的 Impl
};

ImageTaskWindow::ImageTaskWindow() :
    QMainWindow{nullptr},
    d{std::make_unique<ImageTaskWindow::ImageTaskWindowImpl>(this)}
{
}

ImageTaskWindow::~ImageTaskWindow()
{
}

std::vector<ImageTaskWindow*> ImageTaskWindow::getExistingImageTaskWindows()
{
    return ImageTaskWindowImpl::getExistingImageTaskWindows();
}

void ImageTaskWindow::handleCommand(const geometrize::script::Command& command)
{
    d->handleCommand(command);
}

std::string ImageTaskWindow::getCommandHandlerName() const
{
    return d->getCommandHandlerName();
}

void ImageTaskWindow::setCommandHandlerName(const std::string& name)
{
    d->setCommandHandlerName(name);
}

task::ImageTask* ImageTaskWindow::getImageTask()
{
    return d->getImageTask();
}

void ImageTaskWindow::setImageTask(task::ImageTask* task)
{
    d->setImageTask(task);
}

const std::vector<geometrize::ShapeResult>& ImageTaskWindow::getShapes() const
{
    return d->getShapes();
}

void ImageTaskWindow::on_actionExit_triggered()
{
    d->close();
}

void ImageTaskWindow::on_actionLoad_Settings_Template_triggered()
{
    d->loadSettingsTemplate();
}

void ImageTaskWindow::on_actionSave_Settings_Template_triggered()
{
    d->saveSettingsTemplate();
}

void ImageTaskWindow::on_actionReveal_Launch_Window_triggered()
{
    d->revealLaunchWindow();
}

void ImageTaskWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        d->onLanguageChange();
    }
    QMainWindow::changeEvent(event);
}

}

}
