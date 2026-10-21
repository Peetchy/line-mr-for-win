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

// ---- Message type probe ----------------------------------------------------------------------------
//
// Creating %LOCALAPPDATA%\LineMR\type-probe writes type-probe.txt: for each visible message row,
// which row templates (*DelegateWidget) and their children emit signals while the row is drawn,
// what the model returns for other roles, and what the row draws (texts, images, shapes).

class SignalRecorder : public QObject {
	Q_OBJECT

public:
	QMap<QString, int> hits;

public Q_SLOTS:
	void hit()
	{
		QObject *s = sender();
		if (!s)
			return;
		QObject *top = s;
		while (top->parent() && top->isWidgetType() && !static_cast<QWidget *>(top)->isWindow())
			top = top->parent();
		const QMetaMethod m = s->metaObject()->method(senderSignalIndex());
		++hits[QStringLiteral("%1 / %2.%3").arg(QLatin1String(top->metaObject()->className()),
		                                        QLatin1String(s->metaObject()->className()),
		                                        QString::fromLatin1(m.methodSignature()))];
	}
};

void connectAllSignals(QObject *o, SignalRecorder *rec, QList<QMetaObject::Connection> &conns)
{
	const int slot = rec->metaObject()->indexOfSlot("hit()");
	for (const QMetaObject *mo = o->metaObject(); mo && mo->className()[0] != 'Q'; mo = mo->superClass()) {
		for (int i = mo->methodOffset(); i < mo->methodCount(); ++i) {
			if (mo->method(i).methodType() == QMetaMethod::Signal)
				conns << QMetaObject::connect(o, i, rec, slot);
		}
	}
}

void writeTypeProbe()
{
	QFile f(bridgeDirectory() + QStringLiteral("type-probe.txt"));
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		return;
	QTextStream out(&f);
	out.setEncoding(QStringConverter::Utf8);
	QAbstractItemView *view = nullptr;
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (!top->isVisible())
			continue;
		for (auto *v : top->findChildren<QAbstractItemView *>(QStringLiteral("lineMR_list_messages"))) {
			if (v->isVisible())
				view = v;
		}
	}
	if (!view || !view->model()) {
		out << "no visible message list\n";
		return;
	}
	SignalRecorder rec;
	QList<QMetaObject::Connection> conns;
	int templates = 0;
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (!QLatin1StringView(top->metaObject()->className()).endsWith(QLatin1StringView("DelegateWidget")))
			continue;
		++templates;
		connectAllSignals(top, &rec, conns);
		for (QObject *child : top->findChildren<QObject *>())
			connectAllSignals(child, &rec, conns);
	}
	connectAllSignals(view, &rec, conns);
	connectAllSignals(view->model(), &rec, conns);
	out << templates << " templates, " << conns.size() << " signal connections\n";
	QAbstractItemModel *m = view->model();
	for (int r = 0; r < m->rowCount(); ++r) {
		const QModelIndex idx = m->index(r, 0, view->rootIndex());
		const QRect rect = view->visualRect(idx);
		if (!rect.isValid() || !view->viewport()->rect().intersects(rect))
			continue;
		rec.hits.clear();
		const CaptureResult cap = captureRegionText(view->viewport(), rect);
		out << "=== row " << r << QStringLiteral(" [%1,%2 %3x%4]").arg(rect.x()).arg(rect.y()).arg(rect.width()).arg(rect.height())
		    << (cap.skipped ? " SKIPPED" : "") << '\n';
		for (auto it = rec.hits.cbegin(); it != rec.hits.cend(); ++it)
			out << "  signal " << it.key() << " x" << it.value() << '\n';
		for (int role = 0; role <= 1100; ++role) {
			if (role == Qt::DisplayRole || role == 257)
				continue;
			const QVariant v = m->data(idx, role);
			if (v.isValid())
				out << "  role " << role << " = " << shortValue(v) << '\n';
		}
		if (QObject *data = m->data(idx, 257).value<QObject *>()) {
			out << "  data " << data->metaObject()->className();
			for (const QByteArray &name : data->dynamicPropertyNames())
				out << " dyn:" << name << '=' << shortValue(data->property(name.constData()));
			for (QObject *c : data->children())
				out << " child:" << c->metaObject()->className();
			out << '\n';
		}
		for (const CapturedText &t : cap.items)
			out << QStringLiteral("  text (%1,%2 w%3) \"").arg(t.pos.x()).arg(t.pos.y()).arg(t.width) << t.text << "\"\n";
		for (const PaintedImage &pi : cap.images)
			out << QStringLiteral("  image [%1,%2 %3x%4] size %5x%6%7%8%9%10\n")
			           .arg(pi.rect.x()).arg(pi.rect.y()).arg(pi.rect.width()).arg(pi.rect.height())
			           .arg(pi.size.width()).arg(pi.size.height())
			           .arg(pi.alpha ? QStringLiteral(" alpha") : QString())
			           .arg(pi.clearCorners ? QStringLiteral(" clearCorners") : QString())
			           .arg(pi.clipped ? QStringLiteral(" clipped") : QString())
			           .arg(pi.icon.isEmpty() ? QString() : QStringLiteral(" icon=") + pi.icon);
		for (const PaintedShape &s : cap.shapes)
			out << QStringLiteral("  shape [%1,%2 %3x%4] brush %5\n")
			           .arg(s.rect.x()).arg(s.rect.y()).arg(s.rect.width()).arg(s.rect.height()).arg(s.brush);
		if (QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(view)) {
			if (QAccessibleInterface *row = iface->child(r))
				out << "  name \"" << row->text(QAccessible::Name) << "\"\n";
		}
	}
	for (const QMetaObject::Connection &c : std::as_const(conns))
		QObject::disconnect(c);
	a11yLog("[diag] wrote type-probe.txt");
}

void pollTypeProbe()
{
	const QString request = bridgeDirectory() + QStringLiteral("type-probe");
	if (!QFile::exists(request))
		return;
	QFile::remove(request);
	writeTypeProbe();
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
	QObject::connect(timer, &QTimer::timeout, timer, &pollTypeProbe);
	timer->start(kPollIntervalMs);
}

} // namespace line_a11y

#include "a11y_diagnostics.moc"
