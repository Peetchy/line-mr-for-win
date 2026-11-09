#include "line_strings.h"

#include "a11y_log.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QHash>
#include <QtCore/QMetaEnum>
#include <QtCore/QPointer>
#include <QtCore/QThread>
#include <QtGui/QGuiApplication>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

namespace line_a11y {
namespace {

// An engine of LINE's (a Qt Quick window's), else one of the bridge's own: XLT is registered for
// every engine, and the texts come from LINE's translations either way.
QQmlEngine *qmlEngineForStrings()
{
	for (QWindow *w : QGuiApplication::allWindows()) {
		if (auto *quick = qobject_cast<QQuickWindow *>(w)) {
			for (QQuickItem *item : quick->contentItem()->childItems()) {
				if (QQmlEngine *engine = qmlEngine(item))
					return engine;
			}
		}
	}
	static QPointer<QQmlEngine> own;
	if (!own)
		own = new QQmlEngine(qApp);
	return own;
}

QObject *xltObject()
{
	static QPointer<QObject> xlt;
	static int tries = 0;
	if (!xlt && tries < 20) {
		++tries;
		if (QQmlEngine *engine = qmlEngineForStrings())
			xlt = engine->singletonInstance<QObject *>("LineModule", "XLT");
		a11yLog("[strings] LINE's string table %s", xlt ? "found" : "not found");
	}
	return xlt;
}

} // namespace

QString lineText(const char *key)
{
	// LineCall / LineMediaPlayer have their own resources; only LINE.exe is known to have XLT.
	if (inHelperProcess() || !qApp || QThread::currentThread() != qApp->thread())
		return QString();
	static QHash<QByteArray, QString> cache;
	const QByteArray name(key);
	if (const auto it = cache.constFind(name); it != cache.cend())
		return *it;
	QObject *xlt = xltObject();
	if (!xlt)
		return QString();
	const QMetaObject *mo = xlt->metaObject();
	const int index = mo->indexOfEnumerator("XLTKey");
	bool known = false;
	const int value = index >= 0 ? mo->enumerator(index).keyToValue(key, &known) : -1;
	if (!known) {
		a11yLog("[strings] LINE has no text %s", key);
		cache.insert(name, QString());
		return QString();
	}
	QString text;
	QMetaObject::invokeMethod(xlt, "text", Qt::DirectConnection, Q_RETURN_ARG(QString, text), Q_ARG(int, value));
	text = text.trimmed();
	if (!text.isEmpty()) // empty: perhaps not loaded yet, asked again next time
		cache.insert(name, text);
	return text;
}

QStringList lineTextTable()
{
	QStringList lines;
	QObject *xlt = inHelperProcess() ? nullptr : xltObject();
	const QMetaObject *mo = xlt ? xlt->metaObject() : nullptr;
	const int index = mo ? mo->indexOfEnumerator("XLTKey") : -1;
	if (index < 0)
		return lines;
	const QMetaEnum keys = mo->enumerator(index);
	for (int k = 0; k < keys.keyCount(); ++k) {
		QString text;
		QMetaObject::invokeMethod(xlt, "text", Qt::DirectConnection, Q_RETURN_ARG(QString, text), Q_ARG(int, keys.value(k)));
		lines << QString::fromLatin1(keys.key(k)) + QLatin1Char('\t') + text.replace(QLatin1Char('\n'), QLatin1String("\\n"));
	}
	return lines;
}

} // namespace line_a11y
