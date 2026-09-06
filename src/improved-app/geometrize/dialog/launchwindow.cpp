#include <cstdio>
#include <vector>
#include "launchwindow.h"
#include "ui_launchwindow.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QEvent>
#include <QFileInfo>

#include "chaiscript/chaiscript.hpp"

#include "common/constants.h"
#include "common/formatsupport.h"
#include "common/uiactions.h"
#include "common/util.h"
#include "image/imageloader.h"
#include "localization/strings.h"
#include "preferences/globalpreferences.h"
#include "recents/recentitems.h"
#include "script/chaiscriptcreator.h"
#include "script/scriptrunner.h"
#include "serialization/serializationutil.h"
#include "task/imagetask.h"
#include "task/taskutil.h"
#include "version/versioninfo.h"

namespace geometrize
{

namespace dialog
{

class LaunchWindow::LaunchWindowImpl
{
public:
    LaunchWindowImpl(LaunchWindow* pQ) : ui{std::make_unique<Ui::LaunchWindow>()}, q{pQ}, m_engine{script::createLaunchWindowEngine()}
    {
        q->setAttribute(Qt::WA_DeleteOnClose);
        ui->setupUi(q);
        populateUi();
        setupProcessingResolutionCombo();

        // Hide the run script button by default (it's not useful to casual users at the moment, the task queue is better)
        ui->runScriptButton->setHidden(true);

        // Set up recent items list
        if(preferences::getGlobalPreferences().shouldPopulateRecentItemsOnLaunch()) {
            ui->recentsList->setRecentItems(&geometrize::getRecentItems());
        }

        // Set up console widget
        ui->consoleWidget->setEngine(m_engine.get());

        loadConsoleHistory();

        if(preferences::getGlobalPreferences().shouldShowLaunchConsoleByDefault()) {
            setConsoleVisibility(true);
        } else {
            setConsoleVisibility(false);
        }

        connect(ui->actionScript_Console, &QAction::toggled, [this](const bool checked) {
            setConsoleVisibility(checked);
        });

        // Set up templates search and grid
        connect(ui->templateGrid, &dialog::TemplateGrid::signal_templateLoaded, [this](const QString& /*templateFolder*/, const bool /*success*/, const QString& templateName) {
            // 名字由信号直接带出(网格转发按钮侧已解析的 manifest),不再重新解析
            if(!templateName.isEmpty()) {
                ui->templatesSearchEdit->addToCompletionList(templateName);
            }
        });
        connect(ui->templatesSearchEdit, &dialog::CompletionBox::textChanged, [this](const QString& text) {
            ui->templateGrid->setItemFilter(text);
        });

        if(preferences::getGlobalPreferences().shouldPopulateTemplatesOnLaunch()) {
            ui->templateGrid->loadTemplates();
        }

        // Set up the logo
        setupLogo();
    }
    LaunchWindowImpl operator=(const LaunchWindowImpl&) = delete;
    LaunchWindowImpl(const LaunchWindowImpl&) = delete;
    ~LaunchWindowImpl() = default;

    static std::vector<LaunchWindow*> getTopLevelLaunchWindows()
    {
        std::vector<LaunchWindow*> windows;

        QWidgetList topLevelWidgets{QApplication::topLevelWidgets()};
        for(QWidget* w : topLevelWidgets) {
            if(LaunchWindow* lw = dynamic_cast<LaunchWindow*>(w)) {
                windows.push_back(lw);
            }
        }

        return windows;
    }

    void handleCommand(const geometrize::script::Command& command)
    {
        const QString s = QString::fromStdString(command.getCommand());
        if(s == "search_sample_images") {
            ui->templatesSearchEdit->setText(QString::fromStdString(command.getStringArg("search_term")));
        }

        // File menu options
        if(s == "show_file_menu") {
            ui->menuFile->exec(q->mapToGlobal(ui->menuBar->pos()));
        }
        if(s == "file_menu_clear_recent_items") {
            q->on_actionClear_Recents_triggered();
        }
        if(s == "file_menu_open_global_preferences") {
            q->on_actionGlobal_Preferences_triggered();
        }
        if(s == "file_menu_load_global_preferences") {
            q->on_actionLoad_Global_Preferences_triggered();
        }
        if(s == "file_menu_save_global_preferences") {
            q->on_actionSave_Global_Preferences_triggered();
        }
        if(s == "file_menu_exit") {
            q->on_actionExit_triggered();
        }

        if(s == "show_view_menu") {
            ui->menuView->exec(q->mapToGlobal(ui->menuBar->pos()));
        }
        if(s == "view_menu_toggle_script_console") {
            ui->actionScript_Console->toggle();
        }
        if(s == "view_menu_show_script_console") {
            ui->actionScript_Console->setChecked(true);
        }
        if(s == "view_menu_hide_script_console") {
            ui->actionScript_Console->setChecked(false);
        }

        if(s == "show_help_menu") {
            ui->menuHelp->popup(q->mapToGlobal(ui->menuBar->pos()));
        }
        if(s == "help_menu_show_tutorials") {
            ui->actionTutorials->trigger();
        }
        if(s == "help_menu_show_support") {
            ui->actionSupport->trigger();
        }
        if(s == "help_menu_show_about") {
            ui->actionAbout->trigger();
        }

        if(s == "click_open_image") {
            ui->openImageButton->click();
        }
        if(s == "click_open_link") {
            ui->openLinkButton->click();
        }
        if(s == "click_open_task_queue") {
            ui->openTaskQueueButton->click();
        }
    }

    std::string getCommandHandlerName() const
    {
        return q->objectName().toStdString();
    }

    void setCommandHandlerName(const std::string& name)
    {
        q->setObjectName(QString::fromStdString(name));
    }

    void setConsoleVisibility(const bool visible)
    {
        if(ui->actionScript_Console->isChecked() != visible) {
            ui->actionScript_Console->setChecked(visible);
        }
        ui->consoleWidget->setVisible(visible);
    }

    void loadConsoleHistory()
    {
        std::vector<std::string> history{util::readStringVector(util::getAppDataLocation().append("/").append(geometrize::dialog::ScriptConsole::launchConsoleHistoryFilename))};
        ui->consoleWidget->setHistory(history);
    }

    void saveConsoleHistory()
    {
        const std::vector<std::string> history{ui->consoleWidget->getHistory()};
        util::writeStringVector(history, util::getAppDataLocation().append("/").append(geometrize::dialog::ScriptConsole::launchConsoleHistoryFilename));
    }

    void loadGlobalSettingsTemplate()
    {
        const QString path{common::ui::openLoadGlobalSettingsDialog(q)};
        if(path.isEmpty()) {
            return;
        }
        geometrize::preferences::getGlobalPreferences().load(path.toStdString());
    }

    void saveGlobalSettingsTemplate() const
    {
        const QString path{common::ui::openSaveGlobalSettingsDialog(q)};
        if(path.isEmpty()) {
            return;
        }
        geometrize::preferences::getGlobalPreferences().save(path.toStdString());
    }

    void onLanguageChange()
    {
        ui->retranslateUi(q);
        populateUi();
        repopulateProcessingResolutionCombo(); // retranslateUi 重置了下拉项,重填并按偏好恢复选中态
    }

private:
    void populateUi()
    {
        const QString windowTitle{geometrize::strings::Strings::getApplicationName()
                    .append(" ")
                    .append(geometrize::version::getApplicationVersionString())};

        q->setWindowTitle(windowTitle);

        const QString moreResourcesLabel{tr("Get more online", "Text on a button the user presses to open a page containing more resources for using the app, such as extra images, information and help")};
        ui->moreResourcesLink->setText(R"(<a href=")" + constants::MORE_RESOURCES_URL + R"(" style="text-decoration:none;">)" + moreResourcesLabel + R"(</a>)");

        const QString tutorialsLabel{tr("Video tutorials", "Text on a button the user presses to open a page containing video tutorials explaining how to use the app")};
        ui->tutorialsLink->setText(R"(<a href=")" + constants::VIDEO_TUTORIAL_URL + R"(" style="text-decoration:none;">)" + tutorialsLabel + R"(</a>)");

        updateLogoTaskProgress();
    }

    void updateLogoTaskProgress()
    {
        if(!geometrize::preferences::getGlobalPreferences().shouldGeometrizeAppLogoOnLaunch()) {
            ui->logoLabel->setToolTip("");
            return;
        }

        const QString logoToolTip{tr("%1/%2 shapes", "Text showing the number of shapes that have been created to replicate an image so far e.g. 230/300 shapes")
                    .arg(QLocale().toString(static_cast<qlonglong>(m_logoTaskSteps))).arg(QLocale().toString(static_cast<qlonglong>(m_maxLogoTaskSteps)))};
        ui->logoLabel->setToolTip(logoToolTip);
    }

    // 处理分辨率下拉:直写全局偏好的 resize 开关与阈值。所有打开入口(按钮/拖拽/最近列表/
    // URL/模板/批量脚本)汇合到 convertImageToBitmapWithDownscaling,改全局偏好即全部生效,
    // 与全局偏好对话框是同一数据的两个视图,天然同步,无需另开偏好字段。
    void setupProcessingResolutionCombo()
    {
        connect(ui->processingResolutionComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), [this](const int index) {
            onProcessingResolutionChanged(index);
        });

        repopulateProcessingResolutionCombo();
    }

    // 填充下拉项并按当前全局偏好回填选中项;阈值不在预设内(如用户在偏好对话框自定义过)时落到"自定义"
    void repopulateProcessingResolutionCombo()
    {
        QComboBox* combo = ui->processingResolutionComboBox;
        combo->blockSignals(true); // 回填期间不触发 changed 槽,避免启动/切语言时无意义写盘
        combo->clear();

        // 预设档位表:单一事实来源,索引即 data,避免 fill 与 change 两处硬编码索引漂移
        struct ResolutionPreset {
            int limit;
            const char* label;
            const char* tooltip; // 空串 = 无提示
        };
        static const std::vector<ResolutionPreset> presets{
            {256,  "256 × 256",  ""},
            {512,  "512 × 512",  ""},
            {1024, "1024 × 1024", ""},
            {2048, "2048 × 2048", ""},
            {4096, "4096 × 4096", "Suitable for high-resolution sources, but each step is significantly slower and memory usage is higher"},
        };

        for(const ResolutionPreset& preset : presets) {
            combo->addItem(tr(preset.label));
        }
        combo->setItemData(static_cast<int>(presets.size()) - 1, tr(presets.back().tooltip), Qt::ToolTipRole);
        const int unlimitedIndex = static_cast<int>(presets.size());
        combo->addItem(tr("Unlimited"));
        combo->setItemData(unlimitedIndex, tr("Process images at their original size"), Qt::ToolTipRole);
        combo->addItem(tr("Custom"));

        const geometrize::preferences::GlobalPreferences& prefs{geometrize::preferences::getGlobalPreferences()};
        int index = unlimitedIndex + 1; // Custom
        if(!prefs.isImageTaskImageResizeEnabled()) {
            index = unlimitedIndex;
        } else {
            const auto threshold{prefs.getImageTaskResizeThreshold()};
            for(std::size_t i = 0; i < presets.size(); i++) {
                if(threshold.first == presets[i].limit && threshold.second == presets[i].limit) {
                    index = static_cast<int>(i);
                    break;
                }
            }
        }
        combo->setCurrentIndex(index);
        combo->blockSignals(false);
    }

    void onProcessingResolutionChanged(const int index)
    {
        geometrize::preferences::GlobalPreferences& prefs{geometrize::preferences::getGlobalPreferences()};

        // Unlimited/Custom 之外的索引即预设档位表下标(0=256 … 4=4096),与填充逻辑共用同一张表
        static const int presetLimits[]{256, 512, 1024, 2048, 4096};
        constexpr int presetCount{5};
        const int unlimitedIndex = presetCount;

        if(index >= 0 && index < presetCount) {
            prefs.setImageTaskImageResizeEnabled(true);
            prefs.setImageTaskResizeThreshold(presetLimits[index], presetLimits[index]);
        } else if(index == unlimitedIndex) {
            prefs.setImageTaskImageResizeEnabled(false);
        } else {
            return; // "自定义"是展示项,选中时不改偏好
        }

        // 全局偏好 JSON 只在应用退出时的单例 dtor 兜底保存,这里立即落盘,
        // 防止崩溃/强杀时下拉选择丢失(与全局偏好对话框关闭即存的行为一致)
        prefs.save(geometrize::preferences::getGlobalPreferencesConfigPath());
    }

    void setupLogo()
    {
        const QString logoPath{":/logos/logo_small.png"};

        if(!geometrize::preferences::getGlobalPreferences().shouldGeometrizeAppLogoOnLaunch()) {
            ui->logoLabel->setPixmap(logoPath);
        } else {
            QImage image(logoPath);
            image = image.convertToFormat(QImage::Format_RGBA8888); // Note doing this to guarantee format is RGBA8888

            geometrize::Bitmap logoBitmap{image::createBitmap(image)};
            geometrize::Bitmap initialBitmap{logoBitmap.getWidth(), logoBitmap.getHeight(), geometrize::rgba{0, 0, 0, 0}};
            m_logoTask = std::make_unique<task::ImageTask>(logoBitmap, initialBitmap, Qt::QueuedConnection);
            m_logoTask->getPreferences().setShapeTypes(geometrize::ShapeTypes::RECTANGLE);
            m_logoTask->getPreferences().setShapeAlpha(255U);

            ui->logoLabel->setPixmap(image::createPixmap(m_logoTask->getCurrent()));

            connect(m_logoTask.get(), &task::ImageTask::signal_modelDidStep, [this](std::vector<geometrize::ShapeResult> /*results*/) {
                const QPixmap pixmap{image::createPixmap(m_logoTask->getCurrent())};
                ui->logoLabel->setPixmap(pixmap);

                updateLogoTaskProgress();

                m_logoTaskSteps++;
                if(m_logoTaskSteps <= m_maxLogoTaskSteps) {
                    m_logoTask->stepModel();
                }
            });
            m_logoTask->stepModel();
        }
    }

    std::unique_ptr<Ui::LaunchWindow> ui;
    LaunchWindow* q;
    std::unique_ptr<chaiscript::ChaiScript> m_engine;

    std::unique_ptr<task::ImageTask> m_logoTask;
    const std::size_t m_maxLogoTaskSteps{300ULL};
    std::size_t m_logoTaskSteps{0};
};

LaunchWindow::LaunchWindow() :
    QMainWindow(nullptr),
    d{std::make_unique<LaunchWindow::LaunchWindowImpl>(this)}
{
}

LaunchWindow::~LaunchWindow()
{
}

std::vector<LaunchWindow*> LaunchWindow::getTopLevelLaunchWindows()
{
    return LaunchWindowImpl::getTopLevelLaunchWindows();
}

void LaunchWindow::handleCommand(const geometrize::script::Command& command)
{
    d->handleCommand(command);
}

std::string LaunchWindow::getCommandHandlerName() const
{
    return d->getCommandHandlerName();
}

void LaunchWindow::setCommandHandlerName(const std::string& name)
{
    d->setCommandHandlerName(name);
}

void LaunchWindow::dragEnterEvent(QDragEnterEvent* event)
{
    event->acceptProposedAction();
}

void LaunchWindow::dropEvent(QDropEvent* event)
{
    const QList<QUrl> urls{geometrize::format::getUrls(event->mimeData())};
    QStringList scripts;
    QStringList tasks;
    for(const QUrl& url : urls) {
        const QString urlString{url.toString()};

        if(urlString.endsWith(".chai") && url.isLocalFile()) {
            scripts.push_back(url.toLocalFile());
        } else {
            tasks.push_back(urlString);
        }
    }

    if(!scripts.empty()) {
        for(const QString& scriptPath : scripts) {
            geometrize::script::runScript(util::readFileAsString(scriptPath.toStdString()));
        }
    }

    if(!tasks.empty()) {
        util::openTasks(tasks, true);
    }
}

void LaunchWindow::closeEvent(QCloseEvent* /*event*/)
{
    d->saveConsoleHistory();
}

void LaunchWindow::on_actionGlobal_Preferences_triggered()
{
    common::ui::openGlobalPreferences(this);
}

void LaunchWindow::on_actionLoad_Global_Preferences_triggered()
{
    d->loadGlobalSettingsTemplate();
}

void LaunchWindow::on_actionSave_Global_Preferences_triggered()
{
    d->saveGlobalSettingsTemplate();
}

void LaunchWindow::on_actionClear_Recents_triggered()
{
    geometrize::getRecentItems().clear();
}

void LaunchWindow::on_actionExit_triggered()
{
    close();
}

void LaunchWindow::on_openImageButton_clicked()
{
    const QString imagePath{common::ui::openImagePathPickerDialog(this)};
    QFileInfo imageFileInfo(imagePath);
    if(imagePath.isEmpty() || (imageFileInfo.isSymLink() && !QFileInfo(imageFileInfo.symLinkTarget()).exists())) {
        return;
    }

    geometrize::util::openTasks({ imagePath }, true);
}

void LaunchWindow::on_openLinkButton_clicked()
{
    const QUrl url{common::ui::openGetUrlDialog(this)};
    if(url.isEmpty() || !url.isValid()) {
        return;
    }

    geometrize::util::openTasks({ url.toString() }, true);
}

void LaunchWindow::on_runScriptButton_clicked()
{
    const std::string result{common::ui::openGetScriptDialog(this).toStdString()};
    geometrize::script::runScript(util::readFileAsString(result));
}

void LaunchWindow::on_openTaskQueueButton_clicked()
{
    common::ui::openTaskQueueWindow();
}

void LaunchWindow::on_actionTutorials_triggered()
{
    common::ui::openOnlineTutorials();
}

void LaunchWindow::on_actionSupport_triggered()
{
    common::ui::openTechnicalSupport();
}

void LaunchWindow::on_actionAbout_triggered()
{
    common::ui::openAboutPage(this);
}

void LaunchWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        d->onLanguageChange();
    }
    QMainWindow::changeEvent(event);
}

}

}
