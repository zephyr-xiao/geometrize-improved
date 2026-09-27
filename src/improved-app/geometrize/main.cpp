#include "dialog/launchwindow.h"

#include <cassert>
#include <cstdio>
#include <functional>
#include <string>

#include <QApplication>
#include <QEvent>
#include <QIcon>
#include <QLocale>
#include <QObject>
#include <QPixmapCache>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

#include "tabletproximityeventfilter.h"
#include "cli/commandlineparser.h"
#include "common/uiactions.h"
#include "dialog/appsplashscreen.h"
#include "dialog/launchwindow.h"
#include "dialog/welcomewindow.h"
#include "localization/localization.h"
#include "logging/logging.h"
#include "preferences/globalpreferences.h"
#include "test/functionaltestrunner.h"
#include "version/versioninfo.h"

#if defined DATASLINGER_INCLUDED
#include "dataslinger/imageslinger.h"
#endif

namespace {

void setApplicationSettingsFields()
{
    // Do not modify these
    const QString ORGANIZATION_NAME{"Sam Twidale"}; // The development organization's name.
    const QString ORGANIZATION_DOMAIN{"samcodes.co.uk"}; // The development organization's website domain.
    const QString APPLICATION_NAME{"Geometrize"}; // The application name.
    QCoreApplication::setOrganizationName(ORGANIZATION_NAME);
    QCoreApplication::setOrganizationDomain(ORGANIZATION_DOMAIN);
    QCoreApplication::setApplicationName(APPLICATION_NAME);

    QCoreApplication::setApplicationVersion(geometrize::version::getApplicationVersionString()); // This can change
}

void incrementAppLaunchCount()
{
    auto& prefs = geometrize::preferences::getGlobalPreferences();
    prefs.incrementApplicationLaunchCount();
}

void tuneGraphicsCaches()
{
    // 矢量视图的 SvgItem 用 DeviceCoordinateCache,其缓存位图走 Qt 全局 pixmap 缓存,
    // 受 QPixmapCache 上限约束(默认 10MB,按整幅画布的设备像素算只装得下约 5 个图层)。
    // 形状累积到上千后图层数超过该上限,缓存开始抖动——每次重绘都要重新渲染 SVG 文档,
    // 帧耗时从 0.2ms 量级跳到 70ms 量级(离屏实测见 docs/bugfix-triage.md C.3.1)。
    // 128MB 可容纳约 60 个全画布图层(对应上万形状),再往上由 SVG 分块阈值控制增量。
    QPixmapCache::setCacheLimit(128 * 1024); // 参数单位 KB
}

void setLocale(const QStringList& arguments)
{
    const QLocale locale = [&arguments]() {
        const std::string overrideCode{geometrize::cli::getOverrideLocaleCode(arguments)};
        if(!overrideCode.empty()) {
            return QLocale(QString::fromStdString(overrideCode));
        }

        const auto& prefs = geometrize::preferences::getGlobalPreferences();

        // If this is first launch, we override the preferences with the system's preferred UI language
        // Assume launch count was already incremented from an initial value of 0 earlier
        if(prefs.getApplicationLaunchCount() == 1) {
            const QStringList uiLanguages{QLocale().uiLanguages()};

            assert(!uiLanguages.isEmpty() && "No supported UI languages found, will fallback to default settings");
            if(!uiLanguages.isEmpty()) {
                geometrize::setGlobalPreferencesForLocale(QLocale(uiLanguages.front()));
            }
        }

        // Get locale using name built from current preferences
        return geometrize::getGlobalPreferencesLocale();
    }();

    QLocale::setDefault(locale);
    geometrize::setTranslatorsForLocale(locale.bcp47Name().replace("-", "_"));
}

int runAppConsoleMode(QApplication& app)
{
    return geometrize::cli::runApp(app);
}

int runAppGuiModeDesktop(QApplication& app)
{
    geometrize::dialog::sharedSplashInstance().setState(geometrize::dialog::SplashState::STARTING);
    geometrize::dialog::sharedSplashInstance().setState(geometrize::dialog::SplashState::LOADING_LAUNCHER_WINDOW);

    const auto& prefs = geometrize::preferences::getGlobalPreferences();
	if (prefs.shouldShowWelcomeScreenOnLaunch()) {
        geometrize::dialog::sharedSplashInstance().setState(geometrize::dialog::SplashState::FINISHED);
        geometrize::common::ui::openWelcomePage(); // Opens launch window when closed
    } else {
        geometrize::common::ui::openLaunchWindow();
        geometrize::dialog::sharedSplashInstance().setState(geometrize::dialog::SplashState::FINISHED);
    }
    geometrize::logging::logStartupTiming("launchwindow_constructed");
    QTimer::singleShot(0, app.instance(), []() {
        geometrize::logging::logStartupTiming("event_loop_first_turn");
    });

    // Set up the self tests (these will take over the application and run in the background)
    if(geometrize::cli::shouldRunInSelfTestMode(app.arguments())) {
        geometrize::test::setTestScriptDirectories(geometrize::cli::getSelfTestModeScriptDirectories(app.arguments()));
        geometrize::test::runSelfTests();
    }

    return app.exec();
}

std::function<int(QApplication&)> resolveLaunchFunction(const QStringList& arguments)
{
    if(geometrize::cli::shouldRunInConsoleMode(arguments)) {
        return ::runAppConsoleMode;
    }

    return ::runAppGuiModeDesktop;
}

}

int main(int argc, char* argv[])
{
    geometrize::logging::setupLogging();
    geometrize::logging::logStartupTiming("main_start");

    setApplicationSettingsFields();
    incrementAppLaunchCount();

    QApplication app(argc, argv);

    tuneGraphicsCaches();

    // Intercept proximity tablet pen events
    app.installEventFilter(&geometrize::getSharedTabletProximityEventFilterInstance());

#if defined(Q_OS_LINUX) || defined(Q_OS_MAC)
    // Some Linux/Macs taskbars/DEs use the application window icon to set the taskbar icon
    // So we explicitly set the window icon here
    app.setWindowIcon(QIcon(":/logos/logo_small.png"));
#endif

    const QStringList arguments{app.arguments()};
    setLocale(arguments);
    geometrize::logging::logStartupTiming("after_app_and_prefs");

#if defined DATASLINGER_INCLUDED
    geometrize::setupImageSlinger();
    geometrize::setupImageReceiver();
    geometrize::setupSvgShapeSlinger();
#endif

    const auto run = resolveLaunchFunction(arguments);
    return run(app);
}
