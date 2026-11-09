#include "a11y_announce.h"

#include "a11y_log.h"
#include "accessible_quick.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <windows.h>
// WIN32_LEAN_AND_MEAN (set for the whole bridge) leaves out COM, which UI Automation needs.
#include <ole2.h>
#include <uiautomation.h>

namespace line_a11y {
namespace {

void raiseNotification(HWND hwnd, const QString &text, const wchar_t *activityId)
{
	IRawElementProviderSimple *provider = nullptr;
	if (FAILED(UiaHostProviderFromHwnd(hwnd, &provider)) || !provider)
		return;
	BSTR display = SysAllocStringLen(reinterpret_cast<const OLECHAR *>(text.utf16()), UINT(text.size()));
	BSTR activity = SysAllocString(activityId);
	const HRESULT hr = UiaRaiseNotificationEvent(provider, NotificationKind_Other, NotificationProcessing_ImportantAll,
	                                             display, activity);
	SysFreeString(display);
	SysFreeString(activity);
	provider->Release();
	static int logged = 0;
	if (logged++ < 5)
		a11yLog("[announce] %ls: %d chars (hr 0x%08lx)", activityId, int(text.size()), static_cast<unsigned long>(hr));
}

} // namespace

void announce(QWidget *w, const QString &text, bool evenIfInactive)
{
	if (!w || text.isEmpty() || !(evenIfInactive || w->window()->isActiveWindow()))
		return;
	static QString lastText;
	static QElapsedTimer lastTime;
	if (text == lastText && lastTime.isValid() && lastTime.elapsed() < 2000)
		return;
	lastText = text;
	lastTime.start();
	raiseNotification(reinterpret_cast<HWND>(w->window()->winId()), text, L"lineMR.announcement");
}

void announceIncomingCall(const QString &text)
{
	QFile f(bridgeDirectory() + QStringLiteral("incoming-call.txt"));
	if (text.isEmpty() || !f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	f.write(text.toUtf8());
	f.close();
	// Kept open: the add-on may open the event after LINE has created it, or the other way round.
	static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, L"Local\\lineMR.incomingCall");
	const BOOL ok = event && SetEvent(event);
	a11yLog("[announce] incoming call signalled (%d)", int(ok));
}

void requestTranscription(const QString &status, int seconds, const QString &kind)
{
	QFile f(bridgeDirectory() + QStringLiteral("transcribe-request.txt"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	f.write(QStringLiteral("status\t%1\nseconds\t%2\npid\t%3\nkind\t%4\n")
	            .arg(status)
	            .arg(seconds)
	            .arg(GetCurrentProcessId())
	            .arg(kind)
	            .toUtf8());
	f.close();
	// A play request left over from an earlier, abandoned transcription must not start this one.
	transcriptionPlayRequested();
	static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, L"Local\\lineMR.transcribeRequest");
	const BOOL ok = event && SetEvent(event);
	a11yLog("[transcribe] request signalled: %s, %s, %d s (%d)", qPrintable(status), qPrintable(kind), seconds, int(ok));
}

QString transcriptionTarget()
{
	QFile f(bridgeDirectory() + QStringLiteral("transcribe-target.txt"));
	if (!f.open(QIODevice::ReadOnly))
		return QString();
	return QString::fromUtf8(f.readAll()).trimmed();
}

void reportFileFetch(bool saving)
{
	QFile f(bridgeDirectory() + QStringLiteral("transcribe-fetch.txt"));
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		f.write(saving ? "saving" : "failed");
	a11yLog("[transcribe] file save %s", saving ? "started" : "failed");
}

void requestSave(const QString &status, const QString &kind, const QString &name, const QString &text)
{
	QFile body(bridgeDirectory() + QStringLiteral("save-text.txt"));
	if (body.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		body.write(text.toUtf8());
		body.close();
	}
	QFile f(bridgeDirectory() + QStringLiteral("save-request.txt"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	QString safeName = name;
	safeName.replace(u'\t', u' ').replace(u'\n', u' ').replace(u'\r', u' ');
	f.write(QStringLiteral("status\t%1\nkind\t%2\nname\t%3\n").arg(status, kind, safeName).toUtf8());
	f.close();
	static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, L"Local\\lineMR.saveRequest");
	const BOOL ok = event && SetEvent(event);
	a11yLog("[save] request signalled: %s, %s, %d characters (%d)", qPrintable(status), qPrintable(kind), int(text.size()), int(ok));
}

void requestYouTube(const QString &status, const QString &url)
{
	QFile f(bridgeDirectory() + QStringLiteral("youtube-request.txt"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return;
	QString safeUrl = url;
	safeUrl.remove(u'\t').remove(u'\n').remove(u'\r');
	f.write(QStringLiteral("status\t%1\nurl\t%2\n").arg(status, safeUrl).toUtf8());
	f.close();
	static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, L"Local\\lineMR.youtubeRequest");
	const BOOL ok = event && SetEvent(event);
	// The address may be private: only its host.
	a11yLog("[youtube] request signalled: %s, %s (%d)", qPrintable(status), qPrintable(QUrl(url).host()), int(ok));
}

bool transcriptionPlayRequested()
{
	static const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, L"Local\\lineMR.transcribePlay");
	return event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}

void startEndCallListener(QObject *context)
{
	const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, L"Local\\lineMR.endCall");
	if (!event) {
		a11yLog("[call] cannot create the end-call event");
		return;
	}
	auto *timer = new QTimer(context);
	QObject::connect(timer, &QTimer::timeout, context, [event] {
		if (WaitForSingleObject(event, 0) != WAIT_OBJECT_0)
			return;
		// The voice call window (widgets): its End call button, tagged by the bridge.
		for (QWidget *window : QApplication::topLevelWidgets()) {
			QWidget *end = window->isVisible() ? window->findChild<QWidget *>(QStringLiteral("lineMR_call-end")) : nullptr;
			if (!end || !end->isVisible())
				continue;
			QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(end);
			if (QAccessibleActionInterface *action = iface ? iface->actionInterface() : nullptr) {
				action->doAction(QAccessibleActionInterface::pressAction());
				a11yLog("[call] ended the call in %s", window->metaObject()->className());
				return;
			}
		}
		if (!endQuickCall())
			a11yLog("[call] end call requested, but no call window shows one");
	});
	timer->start(200);
}

} // namespace line_a11y
