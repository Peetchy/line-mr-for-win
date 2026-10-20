#include "a11y_diagnostics.h"

#include "a11y_announce.h"
#include "a11y_log.h"
#include "accessible_quick.h"
#include "text_capture.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMap>
#include <QtCore/QMetaProperty>
#include <QtCore/QRegularExpression>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QAccessible>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

namespace line_a11y {
namespace {

constexpr int kPollIntervalMs = 1000;
constexpr int kMaxModelRows = 40;

QString classChain(const QMetaObject *mo)
{
	QStringList names;
	for (; mo && qstrcmp(mo->className(), "QWidget") != 0 && qstrcmp(mo->className(), "QObject") != 0;
	     mo = mo->superClass())
		names << QString::fromLatin1(mo->className());
	return names.join(QLatin1Char('<'));
}

QString shortValue(const QVariant &v)
{
	if (!v.isValid())
		return QStringLiteral("<invalid>");
	if (v.metaType().flags() & QMetaType::PointerToQObject) {
		QObject *o = v.value<QObject *>();
		return o ? QStringLiteral("%1(0x%2)").arg(QLatin1String(o->metaObject()->className()))
		                   .arg(quintptr(o), 0, 16)
		         : QStringLiteral("null");
	}
	if (v.canConvert<QString>()) {
		QString s = v.toString();
		if (s.size() > 160)
			s = s.left(160) + QStringLiteral("…");
		return QLatin1Char('"') + s + QLatin1Char('"');
	}
	return QStringLiteral("<%1>").arg(QLatin1String(v.metaType().name()));
}

// Properties declared by LINE's own classes (everything above the first Q* class).
QString ownProperties(const QObject *o)
{
	QStringList parts;
	for (const QMetaObject *mo = o->metaObject(); mo && mo->className()[0] != 'Q'; mo = mo->superClass()) {
		for (int i = mo->propertyOffset(); i < mo->propertyCount(); ++i) {
			const QMetaProperty p = mo->property(i);
			parts << QStringLiteral("%1=%2").arg(QLatin1String(p.name()), shortValue(p.read(o)));
		}
	}
	return parts.join(QLatin1Char(' '));
}

QString ownSignals(const QObject *o)
{
	QStringList parts;
	for (const QMetaObject *mo = o->metaObject(); mo && mo->className()[0] != 'Q'; mo = mo->superClass()) {
		for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
			const QMetaMethod m = mo->method(i);
			if (m.methodType() == QMetaMethod::Signal)
				parts << QString::fromLatin1(m.methodSignature());
		}
	}
	return parts.join(QLatin1Char(' '));
}

// Every method LINE's own classes declare (invokables and slots can be data getters).
QString ownMethods(const QObject *o)
{
	static const char *const kinds[] = { "method", "signal", "slot", "constructor" };
	QStringList parts;
	for (const QMetaObject *mo = o->metaObject(); mo && mo->className()[0] != 'Q'; mo = mo->superClass()) {
		for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
			const QMetaMethod m = mo->method(i);
			parts << QStringLiteral("%1:%2 %3").arg(QLatin1String(kinds[m.methodType()]), QLatin1String(m.typeName()),
			                                        QString::fromLatin1(m.methodSignature()));
		}
	}
	return parts.join(QStringLiteral(" | "));
}

void dumpModel(QTextStream &out, const QString &indent, QAbstractItemView *view)
{
	QAbstractItemModel *m = view->model();
	if (!m)
		return;
	out << indent << "  model " << m->metaObject()->className() << " rows=" << m->rowCount()
	    << " roles=";
	const auto roles = m->roleNames();
	for (auto it = roles.begin(); it != roles.end(); ++it)
		out << it.key() << ':' << it.value() << ' ';
	out << '\n';
	out << indent << "  view methods: " << ownMethods(view) << '\n';
	out << indent << "  model methods: " << ownMethods(m) << '\n';
	if (QObject *first = m->rowCount() > 0 ? m->data(m->index(0, 0), 257).value<QObject *>() : nullptr)
		out << indent << "  item methods: " << ownMethods(first) << '\n';
	for (int r = 0; r < std::min(m->rowCount(), kMaxModelRows); ++r) {
		const QModelIndex idx = m->index(r, 0);
		const QVariant item = m->data(idx, 257);
		QObject *o = item.value<QObject *>();
		out << indent << "    [" << r << "] display=" << shortValue(m->data(idx, Qt::DisplayRole));
		if (o)
			out << ' ' << o->metaObject()->className() << ' ' << ownProperties(o);
		else
			out << " itemData=" << shortValue(item);
		out << '\n';
	}
}

void dumpWidget(QTextStream &out, QWidget *w, int depth, bool &first)
{
	const QString indent(depth * 2, QLatin1Char(' '));
	const QRect g(w->mapToGlobal(QPoint(0, 0)), w->size());
	out << indent << classChain(w->metaObject());
	if (!w->objectName().isEmpty())
		out << " #" << w->objectName();
	out << QStringLiteral(" [%1,%2 %3x%4]").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
	if (!w->isVisible())
		out << " hidden";
	if (!w->accessibleName().isEmpty())
		out << " accName=\"" << w->accessibleName() << '"';
	if (!w->toolTip().isEmpty())
		out << " tip=\"" << w->toolTip() << '"';
	const QString props = ownProperties(w);
	if (!props.isEmpty())
		out << " {" << props << '}';
	if (w->isVisible() && w->width() > 0) {
		const CaptureResult cap = captureWidgetText(w, false);
		const QString text = capturedTextInReadingOrder(cap);
		if (!text.isEmpty())
			out << " text=\"" << QString(text).replace(QLatin1Char('\n'), QStringLiteral(" / ")) << '"';
		if (cap.glyphRunsWithoutText)
			out << " glyphRuns=" << cap.glyphRunsWithoutText;
		if (cap.pixmaps)
			out << " pixmaps=" << cap.pixmaps;
	}
	if (first) {
		out << "\n" << indent << "  signals: " << ownSignals(w);
		first = false;
	}
	out << '\n';
	if (auto *view = qobject_cast<QAbstractItemView *>(w))
		dumpModel(out, indent, view);
	for (QObject *child : w->children())
		if (auto *cw = qobject_cast<QWidget *>(child); cw && !cw->isWindow())
			dumpWidget(out, cw, depth + 1, first);
}

void dumpAccessible(QTextStream &out, QAccessibleInterface *iface, int depth)
{
	if (!iface || !iface->isValid() || depth > 40)
		return;
	const QString indent(depth * 2, QLatin1Char(' '));
	out << indent << "role=" << int(iface->role()) << " name=\"" << iface->text(QAccessible::Name) << '"';
	const QString value = iface->text(QAccessible::Value);
	if (!value.isEmpty())
		out << " value=\"" << value << '"';
	if (QObject *o = iface->object())
		out << ' ' << o->metaObject()->className();
	out << '\n';
	for (int i = 0; i < iface->childCount(); ++i)
		dumpAccessible(out, iface->child(i), depth + 1);
}

void pollDumpRequest()
{
	const QString request = bridgeDirectory() + QStringLiteral("dump-request");
	if (!QFile::exists(request))
		return;
	QFile::remove(request);
	writeWidgetDump();
}

// Test for the add-on's incoming-call announcement (a real call cannot be produced on demand):
// creating %LOCALAPPDATA%\LineMR\call-announce-test announces its text as an incoming call.
void pollCallAnnounceTest()
{
	QFile request(bridgeDirectory() + QStringLiteral("call-announce-test"));
	if (!request.exists() || !request.open(QIODevice::ReadOnly))
		return;
	const QString text = QString::fromUtf8(request.readAll()).trimmed();
	request.close();
	request.remove();
	announceIncomingCall(text.isEmpty() ? QStringLiteral("Incoming call: test") : text);
}

// Every resource path whose name mentions calls (the call window's icons are named after them).
void writeResourceList(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return;
	QTextStream out(&f);
	out.setEncoding(QStringConverter::Utf8);
	static const QRegularExpression interesting(QStringLiteral("voip|call|ring|incoming"),
	                                            QRegularExpression::CaseInsensitiveOption);
	for (QDirIterator it(QStringLiteral(":/"), QDirIterator::Subdirectories); it.hasNext();) {
		const QString name = it.next();
		if (interesting.match(name).hasMatch())
			out << name << '\n';
	}
}

// Copies of the call windows' style sheets and QML (their layout and texts, for windows that only
// appear during a real call) into resources\.
void writeCallResources(const QString &directory)
{
	static const QRegularExpression interesting(QStringLiteral("(voip|call|toast)[^/]*\\.(style|qml)$"),
	                                            QRegularExpression::CaseInsensitiveOption);
	QDir().mkpath(directory);
	for (QDirIterator it(QStringLiteral(":/"), QDirIterator::Subdirectories); it.hasNext();) {
		const QString name = it.next();
		if (!interesting.match(name).hasMatch())
			continue;
		QFile in(name);
		QFile out(directory + QFileInfo(name).fileName());
		if (in.open(QIODevice::ReadOnly) && out.open(QIODevice::WriteOnly | QIODevice::Truncate))
			out.write(in.readAll());
	}
}

void writeDump(const QString &fileName, const QWidgetList &windows)
{
	const QString path = bridgeDirectory() + fileName;
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		a11yLog("[diag] cannot write %s", qUtf8Printable(path));
		return;
	}
	QTextStream out(&f);
	out.setEncoding(QStringConverter::Utf8);
	breadcrumb("dump %s", qUtf8Printable(fileName));
	if (QWidget *focus = QApplication::focusWidget())
		out << "focus: " << classChain(focus->metaObject()) << " #" << focus->objectName() << '\n';
	if (QWidget *popup = QApplication::activePopupWidget())
		out << "active popup: " << classChain(popup->metaObject()) << '\n';
	for (QWidget *w : windows) {
		if (!w->isVisible())
			continue;
		out << "=== window " << w->metaObject()->className() << " \"" << w->windowTitle() << "\" flags=0x"
		    << QString::number(int(w->windowFlags()), 16) << (w->isActiveWindow() ? " active" : "") << '\n';
		bool first = true;
		dumpWidget(out, w, 0, first);
		out << "--- accessible tree\n";
		dumpAccessible(out, QAccessible::queryAccessibleInterface(w), 0);
		out << '\n';
	}
	int hidden = 0;
	QMap<QByteArray, int> hiddenClasses;
	for (QWidget *w : windows) {
		if (!w->isVisible()) {
			++hidden;
			++hiddenClasses[w->metaObject()->className()];
		}
	}
	for (auto it = hiddenClasses.cbegin(); it != hiddenClasses.cend(); ++it)
		out << "hidden top-level " << it.key() << " x" << it.value() << '\n';
	out << "(" << hidden << " hidden top-level widgets not dumped)\n";
	a11yLog("[diag] wrote %s", qUtf8Printable(path));
}

// The hidden row templates LcListView paints message rows from (state of the last row painted).
void writeDelegateDump(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return;
	QTextStream out(&f);
	out.setEncoding(QStringConverter::Utf8);
	bool first = false;
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (w->isVisible() || !QLatin1StringView(w->metaObject()->className()).endsWith(QLatin1StringView("DelegateWidget")))
			continue;
		out << "=== " << w->metaObject()->className() << '\n';
		dumpWidget(out, w, 0, first);
		if (w->width() > 0 && w->height() > 0) {
			const QString text = capturedTextInReadingOrder(captureWidgetText(w, true));
			out << "  painted: \"" << QString(text).replace(QLatin1Char('\n'), QStringLiteral(" / ")) << "\"\n";
		}
	}
}

} // namespace

void writeWidgetDump()
{
	writeDump(QStringLiteral("widget-dump.txt"), QApplication::topLevelWidgets());
	if (QFile f(bridgeDirectory() + QStringLiteral("quick-dump.txt")); f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		QTextStream out(&f);
		out.setEncoding(QStringConverter::Utf8);
		dumpQuickWindows(out);
	}
	writeDelegateDump(bridgeDirectory() + QStringLiteral("delegates-dump.txt"));
	writeResourceList(bridgeDirectory() + QStringLiteral("resource-list.txt"));
	writeCallResources(bridgeDirectory() + QStringLiteral("resources/"));
}

void writeWindowDump(QWidget *window, const QString &fileName)
{
	if (window)
		writeDump(fileName, { window });
}

void startDiagnostics(QApplication *app)
{
	auto *timer = new QTimer(app);
	QObject::connect(timer, &QTimer::timeout, timer, &pollDumpRequest);
	QObject::connect(timer, &QTimer::timeout, timer, &pollCallAnnounceTest);
	timer->start(kPollIntervalMs);
}

} // namespace line_a11y
