#include "templategrid.h"

#include <algorithm>

#include <QEvent>
#include <QString>
#include <QTimer>

#include "chaiscript/chaiscript.hpp"

#include "common/searchpaths.h"
#include "common/util.h"
#include "dialog/templatebutton.h"
#include "layout/flowlayout.h"
#include "logging/logging.h"
#include "script/chaiscriptcreator.h"

namespace geometrize
{

namespace dialog
{

class TemplateGrid::TemplateGridImpl
{
public:
    TemplateGridImpl(TemplateGrid* pQ) : q{pQ}, m_layout{new layout::FlowLayout(24, 24, 24)}
    {
        q->setLayout(m_layout);
        populateUi();
    }

    /// 模板脚本引擎懒建:点击模板才需要(openTemplate),移出首帧前的关键路径
    chaiscript::ChaiScript& templateLoader()
    {
        if(!m_templateLoader) {
            m_templateLoader = geometrize::script::createDefaultEngine();
        }
        return *m_templateLoader;
    }

    void loadTemplates()
    {
        // Build the list of templates to load
        std::vector<std::string> templateFolders;

        const std::vector<std::string> paths{geometrize::searchpaths::getTemplateSearchPaths()};
        for(const std::string& path : paths) {
            const std::vector<std::string> folders{util::getSubdirectoriesForDirectory(path)};
            templateFolders.insert(templateFolders.end(), folders.begin(), folders.end());
        }

        // Put the folders in a sensible order so it doesn't vary based on filesystem implementation/state
        std::sort(templateFolders.begin(), templateFolders.end());

        // 分批自调度:每批建一组按钮后让事件循环喘一口气(批间可穿插 paint/输入事件),
        // 相比逐个 16ms 定时器把铺满时间从 O(n×16ms) 压到 O(批数×单批耗时),
        // 相比一次性全建避免单次长任务卡住主线程。批大小取"单批主线程成本低于一帧"的量级。
        m_pendingFolders = std::move(templateFolders);
        m_stageOffset = 0;
        QTimer::singleShot(0, Qt::PreciseTimer, q, [this]() { stageNextBatch(); });
    }

    void stageNextBatch()
    {
        constexpr std::size_t batchSize = 16;
        const std::size_t end{(std::min)(m_stageOffset + batchSize, m_pendingFolders.size())};
        for(std::size_t i = m_stageOffset; i < end; i++) {
            addTemplateItem(QString::fromStdString(m_pendingFolders[i]));
        }
        m_stageOffset = end;
        if(end < m_pendingFolders.size()) {
            QTimer::singleShot(0, Qt::PreciseTimer, q, [this]() { stageNextBatch(); });
        } else {
            m_pendingFolders.clear();
            logging::logStartupTiming("templates_staged");
        }
    }

    void setItemFilter(const QString& filter)
    {
        if(filter.isEmpty()) {
            for(TemplateButton* const button : m_buttons) {
                button->show();
            }
            return;
        }

        for(TemplateButton* const button : m_buttons) {
            const QString name{QString::fromStdString(button->getTemplateManifest().getName())};
            const std::vector<std::string> tags{button->getTemplateManifest().getTags()};

            if(name.contains(filter, Qt::CaseInsensitive)) {
                button->show();
            } else {
                button->hide();
            }

            for(const std::string& tag : tags) {
                const QString qTag{QString::fromStdString(tag)};
                if(qTag.contains(filter, Qt::CaseInsensitive)) {
                    button->show();
                }
            }
        }
    }

    TemplateGridImpl operator=(const TemplateGridImpl&) = delete;
    TemplateGridImpl(const TemplateGridImpl&) = delete;
    ~TemplateGridImpl() = default;

    void onLanguageChange()
    {
        populateUi();
    }

private:
    void populateUi()
    {
    }

    void addTemplateItem(const QString& templateFolder)
    {
        TemplateButton* item{new TemplateButton([this]() -> chaiscript::ChaiScript& { return templateLoader(); }, templateFolder)};
        // 转发按钮的加载完成信号(带 manifest 名字):修复上游信号链断裂导致的搜索补全恒空
        q->connect(item, &TemplateButton::signal_templateLoaded, q, &TemplateGrid::signal_templateLoaded);
        m_layout->addWidget(item);
        m_buttons.push_back(item);
    }

    TemplateGrid* q;
    layout::FlowLayout* m_layout;
    std::unique_ptr<chaiscript::ChaiScript> m_templateLoader;
    std::vector<TemplateButton*> m_buttons;
    std::vector<std::string> m_pendingFolders; // 分批铺开的待建队列(loadTemplates 填充,stageNextBatch 消费)
    std::size_t m_stageOffset{0};
};

TemplateGrid::TemplateGrid(QWidget* parent) :
    QWidget(parent),
    d{std::make_unique<TemplateGrid::TemplateGridImpl>(this)}
{
}

TemplateGrid::~TemplateGrid()
{
}

void TemplateGrid::loadTemplates()
{
    d->loadTemplates();
}

void TemplateGrid::setItemFilter(const QString& filter)
{
    d->setItemFilter(filter);
}

void TemplateGrid::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        d->onLanguageChange();
    }
    QWidget::changeEvent(event);
}

}

}
