#include "a11y_log.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMutex>
#include <QtCore/QStandardPaths>

#include <windows.h>

#include <cstdarg>
#include <cstdio>

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
		const QString path = bridgeDirectory() + QStringLiteral("logs/bridge.log");
		const QString old = bridgeDirectory() + QStringLiteral("logs/bridge.old.log");
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

} // namespace line_a11y
