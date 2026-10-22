#include "call_install.h"

#include "a11y_log.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>

#include <windows.h>

namespace line_a11y {
namespace {

constexpr int kSyncIntervalMs = 30000;
const QLatin1StringView kDllName("line_a11y.dll");

// This DLL's own file.
QString selfPath()
{
	HMODULE self = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        reinterpret_cast<LPCWSTR>(&selfPath), &self))
		return QString();
	wchar_t buf[MAX_PATH];
	const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
	return n ? QDir::fromNativeSeparators(QString::fromWCharArray(buf, int(n))) : QString();
}

QByteArray fileHash(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return QByteArray();
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!hash.addData(&f))
		return QByteArray();
	return hash.result();
}

// LINE.exe is bin\current\LINE.exe; the call process lives in Data\plugin\LineCall\<version>.
QString lineCallRoot()
{
	QDir dir(QCoreApplication::applicationDirPath());
	return dir.cdUp() && dir.cdUp() ? dir.filePath(QStringLiteral("Data/plugin/LineCall")) : QString();
}

// Earlier builds moved aside because LineCall had them loaded; gone once LineCall has quit.
void removeOldCopies(const QDir &dir)
{
	const QStringList old = dir.entryList({ kDllName + QStringLiteral(".old*") }, QDir::Files);
	for (const QString &name : old)
		QFile::remove(dir.filePath(name));
}

// Copies `source` to `target` (replacing a loaded copy by moving it aside first).
bool installCopy(const QString &source, const QString &target)
{
	const QString temp = target + QStringLiteral(".new");
	QFile::remove(temp);
	if (!QFile::copy(source, temp)) {
		a11yLog("[call] could not copy the bridge to %s", qUtf8Printable(temp));
		return false;
	}
	if (QFile::exists(target) && !QFile::remove(target)) {
		// Windows before 10 1809 does not delete a DLL a process has loaded, but renames it.
		const QString aside = target + QStringLiteral(".old") + QString::number(QDateTime::currentMSecsSinceEpoch());
		if (!QFile::rename(target, aside)) {
			a11yLog("[call] %s is locked; not replaced", qUtf8Printable(target));
			QFile::remove(temp);
			return false;
		}
	}
	if (!QFile::rename(temp, target)) {
		a11yLog("[call] could not move the bridge into place at %s", qUtf8Printable(target));
		QFile::remove(temp);
		return false;
	}
	return true;
}

void syncCallBridges()
{
	static const QString self = selfPath();
	static const QByteArray selfHash = fileHash(self);
	if (self.isEmpty() || selfHash.isEmpty())
		return;
	const QString root = lineCallRoot();
	if (root.isEmpty())
		return;
	const QDir rootDir(root);
	const QStringList versions = rootDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString &version : versions) {
		QDir dir(rootDir.filePath(version));
		if (!dir.exists(QStringLiteral("LineCall.exe")))
			continue;
		const QString themes = dir.filePath(QStringLiteral("platformthemes"));
		QDir().mkpath(themes);
		const QDir themesDir(themes);
		removeOldCopies(themesDir);
		const QString target = themesDir.filePath(kDllName);
		if (QFileInfo(target).size() == QFileInfo(self).size() && fileHash(target) == selfHash)
			continue;
		const bool existed = QFile::exists(target);
		if (installCopy(self, target))
			a11yLog("[call] %s the bridge for LineCall %s (takes effect when LineCall next starts)",
			        existed ? "updated" : "installed", qUtf8Printable(version));
	}
}

} // namespace

void startCallBridgeSync(QObject *parent)
{
	syncCallBridges();
	auto *timer = new QTimer(parent);
	QObject::connect(timer, &QTimer::timeout, timer, [] { syncCallBridges(); });
	timer->start(kSyncIntervalMs);
}

} // namespace line_a11y
