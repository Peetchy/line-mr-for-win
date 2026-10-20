#include "a11y_announce.h"

#include "a11y_log.h"

#include <QtCore/QElapsedTimer>
#include <QtWidgets/QWidget>

#include <windows.h>
// WIN32_LEAN_AND_MEAN (set for the whole bridge) leaves out COM, which UI Automation needs.
#include <ole2.h>
#include <uiautomation.h>

namespace line_a11y {

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
	const HWND hwnd = reinterpret_cast<HWND>(w->window()->winId());
	IRawElementProviderSimple *provider = nullptr;
	if (FAILED(UiaHostProviderFromHwnd(hwnd, &provider)) || !provider)
		return;
	BSTR display = SysAllocStringLen(reinterpret_cast<const OLECHAR *>(text.utf16()), UINT(text.size()));
	BSTR activity = SysAllocString(L"lineMR.announcement");
	const HRESULT hr = UiaRaiseNotificationEvent(provider, NotificationKind_Other, NotificationProcessing_ImportantAll,
	                                             display, activity);
	SysFreeString(display);
	SysFreeString(activity);
	provider->Release();
	static int logged = 0;
	if (logged++ < 5)
		a11yLog("[announce] %d chars (hr 0x%08lx)", int(text.size()), static_cast<unsigned long>(hr));
}

} // namespace line_a11y
