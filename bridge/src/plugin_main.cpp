// Entry point of line_a11y.dll.
//
// Qt loads this DLL as a "windows" platform-theme plugin from %QT_PLUGIN_PATH%\platformthemes.
// create() bootstraps the accessibility bridge and returns nullptr, so Qt falls back to its
// built-in QWindowsTheme (QWindowsIntegration::createPlatformTheme("windows")).

#include "a11y_diagnostics.h"
#include "a11y_event_filter.h"
#include "a11y_log.h"
#include "accessible_widgets.h"
#include "text_capture.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtGui/QAccessible>
#include <QtGui/qpa/qplatformthemeplugin.h>
#include <QtWidgets/QApplication>

#include <windows.h>

#include <atomic>

namespace line_a11y {
namespace {

std::atomic<bool> g_bootstrapped{ false };

QString hostExecutable()
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	return QString::fromWCharArray(buf, int(n));
}

bool isLineProcess(const QString &exe)
{
	return QFileInfo(exe).fileName().compare(QLatin1String("LINE.exe"), Qt::CaseInsensitive) == 0;
}

// Qt never unloads a plugin whose create() returned nullptr today, but the bridge installs
// callbacks into QtGui, so keep the DLL mapped for the lifetime of the process.
void pinSelf()
{
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                   reinterpret_cast<LPCWSTR>(&pinSelf), &self);
}

// Crash guard: a marker file exists while LINE runs with the bridge and is removed when LINE
// quits normally. If LINE crashed twice in a row, start in safe mode (no rendering of message
// rows, the riskiest part). Tools that kill LINE on purpose delete the marker first.
void checkPreviousSessions(QApplication *app)
{
	const QString marker = bridgeDirectory() + QStringLiteral("session.running");
	const QString counter = bridgeDirectory() + QStringLiteral("crash-count");
	int crashes = 0;
	if (QFile::exists(marker)) {
		QFile c(counter);
		if (c.open(QIODevice::ReadOnly))
			crashes = c.readAll().trimmed().toInt();
		++crashes;
		a11yLog("[guard] previous LINE session did not end normally (%d in a row)", crashes);
	}
	if (QFile c(counter); c.open(QIODevice::WriteOnly | QIODevice::Truncate))
		c.write(QByteArray::number(crashes));
	if (QFile m(marker); m.open(QIODevice::WriteOnly | QIODevice::Truncate))
		m.write(QByteArray::number(qint64(GetCurrentProcessId())));
	if (crashes >= 2) {
		setSafeMode(true);
		a11yLog("[guard] safe mode: message rows are not rendered");
	}
	QObject::connect(app, &QCoreApplication::aboutToQuit, [marker, counter] {
		QFile::remove(marker);
		QFile::remove(counter);
	});
}

void startBridge()
{
	auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
	if (!app) {
		a11yLog("[bridge] application is not a QApplication; bridge not started");
		return;
	}
	QAccessible::removeFactory(lineAccessibleFactory);
	QAccessible::installFactory(lineAccessibleFactory);
	enforceAccessibilityHandler("startBridge");
	checkPreviousSessions(app);
	installEventFilter(app);
	startDiagnostics(app);
	a11yLog("[bridge] started on %s (accessibility %s)", app->metaObject()->className(),
	        QAccessible::isActive() ? "active" : "inactive");
}

void bootstrap()
{
	bool expected = false;
	if (!g_bootstrapped.compare_exchange_strong(expected, true))
		return;
	pinSelf();
	a11yLog("[plugin] line_a11y loaded into %s (pid %lu), Qt runtime %s, data dir %s",
	        qUtf8Printable(hostExecutable()), GetCurrentProcessId(), qVersion(),
	        qUtf8Printable(bridgeDirectory()));
	QAccessible::installFactory(lineAccessibleFactory);
	// We run inside the QGuiApplication constructor; finish once the event loop is up.
	QMetaObject::invokeMethod(QCoreApplication::instance(), [] { startBridge(); }, Qt::QueuedConnection);
}

} // namespace
} // namespace line_a11y

class LineA11yPlugin : public QPlatformThemePlugin {
	Q_OBJECT
	Q_PLUGIN_METADATA(IID QPlatformThemeFactoryInterface_iid FILE "line_a11y.json")

public:
	QPlatformTheme *create(const QString &key, const QStringList &) override
	{
		// QT_PLUGIN_PATH may reach other Qt programs; stay inert outside LINE.
		if (QCoreApplication::instance() && line_a11y::isLineProcess(line_a11y::hostExecutable()))
			line_a11y::bootstrap();
		Q_UNUSED(key);
		return nullptr;
	}
};

#include "plugin_main.moc"
