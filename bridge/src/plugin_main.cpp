// Entry point of line_a11y.dll.
//
// Qt loads this DLL as a "windows" platform-theme plugin from <program folder>\platformthemes:
// LINE.exe (bin\current) and LINE's helper processes in Data\plugin\<name>\<version>: LineCall.exe
// (incoming-call notification and call windows) and LineMediaPlayer.exe (photo and video viewer).
// create() bootstraps the accessibility bridge and returns nullptr, so Qt falls back to its
// built-in QWindowsTheme (QWindowsIntegration::createPlatformTheme("windows")).

#include "a11y_announce.h"
#include "a11y_diagnostics.h"
#include "a11y_event_filter.h"
#include "a11y_log.h"
#include "accessible_widgets.h"
#include "call_install.h"
#include "text_capture.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtGui/QAccessible>
#include <QtGui/qpa/qplatformthemeplugin.h>
#include <QtWidgets/QApplication>

#include <windows.h>
#include <dbghelp.h>

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
	const QString name = QFileInfo(exe).fileName();
	return name.compare(QLatin1String("LINE.exe"), Qt::CaseInsensitive) == 0
	    || name.compare(QLatin1String("LineCall.exe"), Qt::CaseInsensitive) == 0
	    || name.compare(QLatin1String("LineMediaPlayer.exe"), Qt::CaseInsensitive) == 0;
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
	const bool crashed = QFile::exists(marker);
	openBreadcrumbs(crashed);
	if (crashed) {
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

// Crash dumps: LINE's heap-corruption crashes (0xc0000374) leave no dump (WER LocalDumps needs
// administrator rights). Heap corruption is raised as an exception, so a vectored handler sees it
// first and writes %LOCALAPPDATA%\LineMR\crash.dmp. Everything it needs is prepared here, at start-up:
// the heap is not to be trusted at that point.
using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                           PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_writeDump = nullptr;
wchar_t g_dumpPath[MAX_PATH];

LONG CALLBACK writeCrashDump(EXCEPTION_POINTERS *info)
{
	if (info->ExceptionRecord->ExceptionCode != 0xC0000374 /* STATUS_HEAP_CORRUPTION */)
		return EXCEPTION_CONTINUE_SEARCH;
	static LONG written = 0;
	if (InterlockedExchange(&written, 1))
		return EXCEPTION_CONTINUE_SEARCH;
	const HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		MINIDUMP_EXCEPTION_INFORMATION exception{ GetCurrentThreadId(), info, FALSE };
		g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
		            MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo
		                          | MiniDumpWithUnloadedModules),
		            &exception, nullptr, nullptr);
		CloseHandle(file);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

void installCrashDumpWriter()
{
	// LINE ships dbghelp.dll next to LINE.exe.
	if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
		g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
	const QString path = QDir::toNativeSeparators(
		bridgeDirectory() + (inHelperProcess() ? helperProcessName() + QStringLiteral("-crash.dmp") : QStringLiteral("crash.dmp")));
	if (!g_writeDump || path.size() >= MAX_PATH)
		return;
	path.toWCharArray(g_dumpPath);
	g_dumpPath[path.size()] = L'\0';
	AddVectoredExceptionHandler(1, writeCrashDump);
}

// LineCall.exe, LineMediaPlayer.exe: the crash guard, breadcrumbs and dump requests belong to
// LINE.exe (the processes run at the same time and would share their files).
void startHelperBridge(QCoreApplication *app)
{
	QAccessible::removeFactory(lineAccessibleFactory);
	QAccessible::installFactory(lineAccessibleFactory);
	enforceAccessibilityHandler("startBridge");
	installCrashDumpWriter();
	installEventFilter(app);
	if (helperProcessName() == QLatin1StringView("linecall"))
		startEndCallListener(app);
	startHelperDiagnostics(app);
	a11yLog("[bridge] started on %s in %s (accessibility %s)", app->metaObject()->className(),
	        qUtf8Printable(helperProcessName()), QAccessible::isActive() ? "active" : "inactive");
}

void startBridge()
{
	if (inHelperProcess()) {
		startHelperBridge(QCoreApplication::instance());
		return;
	}
	auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
	if (!app) {
		a11yLog("[bridge] application is not a QApplication; bridge not started");
		return;
	}
	QAccessible::removeFactory(lineAccessibleFactory);
	QAccessible::installFactory(lineAccessibleFactory);
	enforceAccessibilityHandler("startBridge");
	checkPreviousSessions(app);
	installCrashDumpWriter();
	installEventFilter(app);
	startDiagnostics(app);
	startCallBridgeSync(app);
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
		// QT_PLUGIN_PATH may reach other Qt programs; stay inert outside LINE and its helper processes.
		if (QCoreApplication::instance() && line_a11y::isLineProcess(line_a11y::hostExecutable()))
			line_a11y::bootstrap();
		Q_UNUSED(key);
		return nullptr;
	}
};

#include "plugin_main.moc"
