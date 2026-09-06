#pragma once

#include <string>

namespace geometrize
{

namespace logging
{

/**
 * @brief setupLogging Installs a log message handler. Logging in Geometrize uses the Qt QLoggingCategory logging framework
 * See: https://doc.qt.io/qt/qtglobal.html#qInstallMessageHandler
 */
void setupLogging();

/**
 * @brief logStartupTiming Appends a labeled timing line to the startup timing log.
 * Used to measure launch-path latency: the timer starts at the first call and every
 * subsequent call records ms since then. Output goes to a file (not stderr/qInfo)
 * because release builds are GUI-subsystem and the message handler routes to script
 * console widgets that do not exist yet during early startup.
 * @param label A short stable identifier for the measurement point.
 */
void logStartupTiming(const std::string& label);

}

}
