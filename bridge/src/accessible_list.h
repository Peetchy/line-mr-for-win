// Accessibility for LINE's LcListView (chat list, friend list, chat messages, ...).
//
// LcListView does not create a widget per row; Qt's stock QAccessibleTable therefore exposes empty
// cells (the model's display data is an opaque object). Row texts come from, in order:
//  1. the model's per-row data object (role Qt::UserRole + 1, "itemData"): ChatData, ProfileData,
//     GroupData, SectionData are QObjects whose Q_PROPERTYs hold name, last message, time, counts;
//  2. what LINE paints for the row (chat messages: ChatMessageData has no properties). The row's
//     rectangle of the viewport is rendered into the text-recording paint device
//     (captureRegionText()); unlike the macOS bridge this needs no memory offsets.

#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtGui/QAccessible>
#include <QtWidgets/QAccessibleWidget>

QT_BEGIN_NAMESPACE
class QAbstractItemView;
QT_END_NAMESPACE

namespace line_a11y {

bool isLineListView(const QWidget *w);

// LcListView as a flat list of row items. Items stand for row *positions* (their model index is
// resolved on every query), so they survive LINE's constant model churn.
class LineAccessibleList : public QAccessibleWidget {
public:
	explicit LineAccessibleList(QWidget *w);
	~LineAccessibleList() override;

	QString text(QAccessible::Text t) const override;
	QAccessible::State state() const override;

	int childCount() const override;
	QAccessibleInterface *child(int index) const override;
	int indexOfChild(const QAccessibleInterface *child) const override;
	QAccessibleInterface *focusChild() const override;
	QAccessibleInterface *childAt(int x, int y) const override;

	QAccessibleInterface *itemAt(int row) const;
	int rowCount() const;
	bool isRowSelected(int row) const;
	QAbstractItemView *view() const;

	// Sends a Focus event for the current row's item when the list has keyboard focus (after
	// scrolling it into view: rows only have text while LINE can paint them).
	void notifyCurrentRow();

	// Painted text of `row`, remembered after the row scrolls out of view (until the model is
	// reset or rows go away: LINE reuses row objects for the next chat).
	QString paintedRowText(int row, QObject *data) const;

	// Makes the newest visible row current (messages start at the bottom).
	void focusLastVisibleRow();

private:
	void connectView();
	// Drops items past the end once the model has settled after rows went away.
	void schedulePrune() const;

	mutable QHash<int, QAccessible::Id> m_items;
	struct PaintedText {
		QString text;
		qint64 at = 0;
	};
	mutable QHash<quintptr, PaintedText> m_painted;
	mutable bool m_prunePending = false;
	QList<QMetaObject::Connection> m_connections;
};

} // namespace line_a11y
