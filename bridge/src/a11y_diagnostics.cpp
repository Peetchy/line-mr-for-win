#include "a11y_diagnostics.h"

#include "a11y_log.h"
#include "text_capture.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QFile>
#include <QtCore/QMetaProperty>
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

} // namespace

void writeWidgetDump()
{
	const QString path = bridgeDirectory() + QStringLiteral("widget-dump.txt");
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		a11yLog("[diag] cannot write %s", qUtf8Printable(path));
		return;
	}
	QTextStream out(&f);
	out.setEncoding(QStringConverter::Utf8);
	const QWidgetList tops = QApplication::topLevelWidgets();
	for (QWidget *w : tops) {
		if (!w->isVisible())
			continue;
		out << "=== window " << w->metaObject()->className() << " \"" << w->windowTitle() << "\"\n";
		bool first = true;
		dumpWidget(out, w, 0, first);
		out << "--- accessible tree\n";
		dumpAccessible(out, QAccessible::queryAccessibleInterface(w), 0);
		out << '\n';
	}
	int hidden = 0;
	for (QWidget *w : tops)
		hidden += w->isVisible() ? 0 : 1;
	out << "(" << hidden << " hidden top-level widgets not dumped)\n";
	a11yLog("[diag] wrote %s", qUtf8Printable(path));
}

void startDiagnostics(QApplication *app)
{
	auto *timer = new QTimer(app);
	QObject::connect(timer, &QTimer::timeout, timer, &pollDumpRequest);
	timer->start(kPollIntervalMs);
}

} // namespace line_a11y
