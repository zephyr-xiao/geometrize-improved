#include "imagetaskexportwidget.h"
#include "ui_imagetaskexportwidget.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>

#include "geometrize/shaperesult.h"
#include "geometrize/exporter/shapearrayexporter.h"
#include "geometrize/exporter/svgexporter.h"

#include "common/uiactions.h"
#include "common/util.h"
#include "exporter/gifexporter.h"
#include "exporter/imageexporter.h"
#include "exporter/shapedataexporter.h"
#include "exporter/webpageexporter.h"
#include "task/imagetask.h"

namespace geometrize
{

namespace dialog
{

class ImageTaskExportWidget::ImageTaskExportWidgetImpl
{
public:
    ImageTaskExportWidgetImpl(ImageTaskExportWidget* pQ) : m_task{nullptr}, m_shapes{nullptr}, q{pQ}, ui{std::make_unique<Ui::ImageTaskExportWidget>()}
    {
        ui->setupUi(q);
        populateUi();
    }
    ~ImageTaskExportWidgetImpl() = default;
    ImageTaskExportWidgetImpl operator=(const ImageTaskExportWidgetImpl&) = delete;
    ImageTaskExportWidgetImpl(const ImageTaskExportWidgetImpl&) = delete;

    void setImageTask(const task::ImageTask* task, const std::vector<geometrize::ShapeResult>* shapes)
    {
        m_task = task;
        m_shapes = shapes;
    }

    void saveSVG() const
    {
        if(!m_task || !m_shapes) {
            showExportMisconfiguredMessage();
            return;
        }

        const QString path{common::ui::openSaveSVGPathPickerDialog(q)};
        if(path.isEmpty()) {
            return;
        }

        const std::string data{geometrize::exporter::exportSVG(*m_shapes, m_task->getCurrent().getWidth(), m_task->getCurrent().getHeight())};
        util::writeStringToFile(data, path.toStdString());
    }

    void saveRasterizedSVG() const
    {
        if(!m_task || !m_shapes) {
            showExportMisconfiguredMessage();
            return;
        }

        const QString path{common::ui::openSaveRasterizedSVGPathPickerDialog(q)};
        if(path.isEmpty()) {
            return;
        }

        // 按用户倍率在放大画布上重放形状矢量:形状坐标不变,放大由 SVG viewBox→输出尺寸映射完成
        const std::uint32_t scaleFactor{static_cast<std::uint32_t>(std::max(1, ui->imageScaleSpinBox->value()))};
        const std::uint32_t width{m_task->getCurrent().getWidth()};
        const std::uint32_t height{m_task->getCurrent().getHeight()};
        geometrize::exporter::exportRasterizedSvg(
                    *m_shapes,
                    width,
                    height,
                    width * scaleFactor,
                    height * scaleFactor,
                    path.toStdString());
    }

    void saveRasterizedSVGs() const
    {
        if(!m_task || !m_shapes) {
            showExportMisconfiguredMessage();
            return;
        }

        const QString path{common::ui::openSaveRasterizedSVGsPathPickerDialog(q)};
        if(path.isEmpty()) {
            return;
        }

        // 按用户倍率在放大画布上重放形状矢量,逐步前缀各存一帧(导出器内部已做增量渲染+帧数上限)
        const std::uint32_t scaleFactor{static_cast<std::uint32_t>(std::max(1, ui->imageScaleSpinBox->value()))};
        const std::uint32_t width{m_task->getCurrent().getWidth()};
        const std::uint32_t height{m_task->getCurrent().getHeight()};

        runExportInBackground(tr("Exporting image sequence..."),
            [shapes = *m_shapes, width, height, scaleFactor, targetDir = path.toStdString()]() {
                geometrize::exporter::exportRasterizedSvgs(shapes, width, height,
                    width * scaleFactor, height * scaleFactor,
                    targetDir, "exported_image", ".png");
            });
    }

    void saveGeometryData() const
    {
        if(!m_shapes) {
            showExportMisconfiguredMessage();
            return;
        }

        const QString path{common::ui::openSaveGeometryDataPathPickerDialog(q)};
        if(path.isEmpty()) {
            return;
        }

        geometrize::exporter::ShapeDataFormat format = exporter::ShapeDataFormat::JSON;
        if(path.endsWith("json")) {
            format = geometrize::exporter::ShapeDataFormat::JSON;
        } else if(path.endsWith("txt")) {
            format = geometrize::exporter::ShapeDataFormat::CUSTOM_ARRAY;
        }

        const std::string data{geometrize::exporter::exportShapeData(*m_shapes, format)};
        util::writeStringToFile(data, path.toStdString());
    }

    void saveGIF() const
    {
        if(!m_task || !m_shapes) {
            showExportMisconfiguredMessage();
            return;
        }

        const QString path{common::ui::openSaveGIFPathPickerDialog(q)};
        if(path.isEmpty()) {
            return;
        }

        // 参数与控件默认值对齐:每 20 shape 一帧、20FPS(GIF 延迟下限 20ms,上限 50)、x1 输出、无限循环、末帧 2 秒
        const std::size_t frameStep{static_cast<std::size_t>(std::max(1, ui->frameStepSpinBox->value()))};
        const std::uint32_t frameDelayMs{static_cast<std::uint32_t>(1000 / std::max(1, ui->frameRateSpinBox->value()))};
        const std::uint32_t scaleFactor{static_cast<std::uint32_t>(std::max(1, ui->scaleFactorSpinBox->value()))};
        const std::uint32_t loopCount{ui->loopGifCheckbox->isChecked() ? 0U : 1U};
        const std::uint32_t endPauseMs{ui->endPauseCheckbox->isChecked() ? 2000U : 0U};

        const std::uint32_t width{m_task->getCurrent().getWidth()};
        const std::uint32_t height{m_task->getCurrent().getHeight()};

        auto frameSkipPredicate = [frameStep](const std::size_t frameIdx) {
            return frameIdx % frameStep != 0; // 保留每第 N 个形状处取帧
        };

        // 后台执行:导出走 QtConcurrent 线程,UI 弹进度框。shapes 以 shared_ptr 列表浅拷贝
        // 持有(形状本体共享,不随任务销毁失效)
        runExportInBackground(tr("Exporting GIF..."),
            [shapes = *m_shapes, width, height, scaleFactor, frameSkipPredicate, frameDelayMs, loopCount, endPauseMs, filePath = path.toStdString()]() {
                geometrize::exporter::exportGIF(shapes, width, height,
                    width * scaleFactor, height * scaleFactor,
                    frameSkipPredicate, filePath, frameDelayMs, loopCount, endPauseMs);
            });
    }

    void saveHTML5WebpageButton() const
    {
        if(!m_shapes) {
            showExportMisconfiguredMessage();
            return;
        }

        const QString path{common::ui::openSaveCanvasPathPickerDialog(q)};
        if(path.isEmpty()) {
            return;
        }

        const std::string pageSource{geometrize::exporter::exportCanvasWebpage(*m_shapes)};
        util::writeStringToFile(pageSource, path.toStdString());
    }

    void saveWebGLWebpageButton() const
    {
        if(!m_shapes) {
            showExportMisconfiguredMessage();
            return;
        }

        const QString path{common::ui::openSaveWebGLPathPickerDialog(q)};
        if(path.isEmpty()) {
            return;
        }

        const std::string pageSource{geometrize::exporter::exportWebGLWebpage(*m_shapes)};
        util::writeStringToFile(pageSource, path.toStdString());
    }

    void onLanguageChange()
    {
        ui->retranslateUi(q);
        populateUi();
    }

private:
    void populateUi()
    {
    }

    void showExportMisconfiguredMessage() const
    {
        QMessageBox::warning(q, tr("Failed to run exporter", "Title of error message shown when an attempt to save/export a file failed"),
                             tr("Failed to run exporter. Exporter was misconfigured.", "Error message text shown when an attempt to save/export a file failed"));
    }

    /// 把耗时导出挂到 QtConcurrent 线程:期间弹不可取消的进度框并禁用导出按钮,
    /// 完成后自动恢复。widget 是 WA_DeleteOnClose 链上的一环,watcher 以 q 为 parent、
    /// 回调以 QPointer 守护,窗口先关也不会拍悬空对象。
    template<typename Func>
    void runExportInBackground(const QString& message, Func&& work) const
    {
        QPointer<ImageTaskExportWidget> guard{q};
        auto* dialog = new QProgressDialog(message, QString(), 0, 0, q);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->setCancelButton(nullptr);
        dialog->setMinimumDuration(0);
        dialog->show();

        auto* watcher = new QFutureWatcher<void>(q);
        q->connect(watcher, &QFutureWatcher<void>::finished, q, [guard, dialog]() {
            if(guard.isNull()) {
                return;
            }
            dialog->deleteLater();
            guard->setEnabled(true);
        });

        q->setEnabled(false);
        watcher->setFuture(QtConcurrent::run(std::forward<Func>(work)));
    }

    const geometrize::task::ImageTask* m_task;
    const std::vector<geometrize::ShapeResult>* m_shapes;

    ImageTaskExportWidget* q;
    std::unique_ptr<Ui::ImageTaskExportWidget> ui;
};

ImageTaskExportWidget::ImageTaskExportWidget(QWidget* parent) :
    QWidget{parent},
    d{std::make_unique<ImageTaskExportWidget::ImageTaskExportWidgetImpl>(this)}
{
}

ImageTaskExportWidget::~ImageTaskExportWidget()
{
}

void ImageTaskExportWidget::setImageTask(const task::ImageTask* task, const std::vector<geometrize::ShapeResult>* shapes)
{
    d->setImageTask(task, shapes);
}

void ImageTaskExportWidget::on_saveImageButton_clicked()
{
    d->saveRasterizedSVG();
}

void ImageTaskExportWidget::on_saveImagesButton_clicked()
{
    d->saveRasterizedSVGs();
}

void ImageTaskExportWidget::on_saveSVGButton_clicked()
{
    d->saveSVG();
}

void ImageTaskExportWidget::on_saveGeometryDataButton_clicked()
{
    d->saveGeometryData();
}

void ImageTaskExportWidget::on_saveGIFButton_clicked()
{
    d->saveGIF();
}

void ImageTaskExportWidget::on_saveHTML5WebpageButton_clicked()
{
    d->saveHTML5WebpageButton();
}

void ImageTaskExportWidget::on_saveWebGLWebpageButton_clicked()
{
    d->saveWebGLWebpageButton();
}

void ImageTaskExportWidget::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        d->onLanguageChange();
    }
    QWidget::changeEvent(event);
}

}

}
