#include "logging/logging.h"

#include <chrono>
#include <fstream>
#include <string>

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtGlobal>
#include <QLoggingCategory>
#include <QMessageLogContext>
#include <QString>

#include "common/util.h"

namespace
{

void messageHandler(QtMsgType, const QMessageLogContext&, const QString& msg)
{
    const QByteArray localMsg = msg.toLocal8Bit();
    geometrize::util::printToAllScriptConsoleWidgets("Log message: " + localMsg.toStdString());
}

}

// 启动打点的起算点:首次调用时初始化(main 入口附近),后续打点均为相对毫秒
QElapsedTimer& startupTimer()
{
    static QElapsedTimer timer;
    static bool started = false;
    if(!started) {
        timer.start();
        started = true;
    }
    return timer;
}

std::string startupTimingLogPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).toStdString() + "/startup_timing.log";
}

namespace geometrize
{

namespace logging
{

void setupLogging()
{
    qInstallMessageHandler(&messageHandler);
}

void logStartupTiming(const std::string& label)
{
    QElapsedTimer& timer = startupTimer();
    if(!timer.isValid()) {
        return;
    }
    const std::string filePath{startupTimingLogPath()};
    const QFileInfo info(QString::fromStdString(filePath));
    const QDir dir(info.absoluteDir());
    if(!dir.exists() && !dir.mkpath(dir.absolutePath())) {
        return;
    }
    std::ofstream output(filePath, std::ios::app);
    output << QDateTime::currentDateTime().toString(Qt::ISODate).toStdString()
           << "," << label << "," << timer.elapsed() << "\n";
}

}

}
