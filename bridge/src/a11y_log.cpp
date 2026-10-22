#include "a11y_log.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QStandardPaths>

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace line_a11y {
namespace {

QMutex g_logMutex;
FILE *g_logFile = nullptr;

QString computeDirectory()
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
	QString base = (n > 0 && n < MAX_PATH) ? QString::fromWCharArray(buf, int(n)) : QDir::tempPath();
	QString dir = QDir::cleanPath(base + QStringLiteral("/LineMR")) + QLatin1Char('/');
	QDir().mkpath(dir + QStringLiteral("logs"));
	return dir;
}

FILE *logFile()
{
	if (!g_logFile) {
		// LINE.exe and its helper processes run at the same time: one log each.
		const QString name = QStringLiteral("logs/") + (inHelperProcess() ? helperProcessName() : QStringLiteral("bridge"));
		const QString path = bridgeDirectory() + name + QStringLiteral(".log");
		const QString old = bridgeDirectory() + name + QStringLiteral(".old.log");
		QFile::remove(old);
		QFile::rename(path, old);
		g_logFile = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"ab");
	}
	return g_logFile;
}

} // namespace

const QString &bridgeDirectory()
{
	static const QString dir = computeDirectory();
	return dir;
}

const QString &helperProcessName()
{
	static const QString name = [] {
		wchar_t buf[MAX_PATH];
		const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
		const QString exe = QString::fromWCharArray(buf, int(n));
		const QString base = exe.mid(exe.lastIndexOf(QLatin1Char('\\')) + 1).toLower();
		return base == QLatin1StringView("line.exe") ? QString() : base.chopped(4); // without ".exe"
	}();
	return name;
}

bool inHelperProcess()
{
	return !helperProcessName().isEmpty();
}

void a11yLogString(const QString &line)
{
	QMutexLocker lock(&g_logMutex);
	FILE *f = logFile();
	if (!f)
		return;
	const QByteArray stamp = QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss.zzz")).toUtf8();
	const QByteArray text = line.toUtf8();
	fprintf(f, "%s [%lu] %s\n", stamp.constData(), GetCurrentThreadId(), text.constData());
	fflush(f);
}

void a11yLog(const char *fmt, ...)
{
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	a11yLogString(QString::fromUtf8(buf));
}

// ---- Crash breadcrumbs ---------------------------------------------------------------------------

namespace {

constexpr int kCrumbCount = 64;
constexpr int kCrumbSize = 128;

struct Crumbs {
	LONG next;
	char entries[kCrumbCount][kCrumbSize];
};

Crumbs *g_crumbs = nullptr;

} // namespace

void openBreadcrumbs(bool logPrevious)
{
	if (g_crumbs)
		return;
	const QString path = bridgeDirectory() + QStringLiteral("breadcrumbs.bin");
	const HANDLE file = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
	                                FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(Crumbs), nullptr);
	CloseHandle(file);
	if (!mapping)
		return;
	auto *crumbs = static_cast<Crumbs *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Crumbs)));
	CloseHandle(mapping); // the view keeps the mapping alive for the rest of the process
	if (!crumbs)
		return;
	if (logPrevious) {
		a11yLog("[guard] last bridge actions of the previous session (oldest first):");
		const LONG next = crumbs->next;
		for (int i = 0; i < kCrumbCount; ++i) {
			char *entry = crumbs->entries[(next + i) % kCrumbCount];
			entry[kCrumbSize - 1] = '\0';
			if (entry[0])
				a11yLog("[guard]   %s", entry);
		}
	}
	memset(crumbs, 0, sizeof(Crumbs));
	g_crumbs = crumbs;
}

void breadcrumb(const char *fmt, ...)
{
	if (!g_crumbs)
		return;
	const LONG slot = (InterlockedIncrement(&g_crumbs->next) - 1) % kCrumbCount;
	char *entry = g_crumbs->entries[slot < 0 ? slot + kCrumbCount : slot];
	SYSTEMTIME t;
	GetLocalTime(&t);
	const int n = snprintf(entry, kCrumbSize, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(entry + n, size_t(kCrumbSize - n), fmt, ap);
	va_end(ap);
}

} // namespace line_a11y
