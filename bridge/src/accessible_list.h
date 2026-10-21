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
#include <QtCore/QPersistentModelIndex>
#include <QtCore/QPointer>
#include <QtGui/QAccessible>
#include <QtWidgets/QAccessibleWidget>

QT_BEGIN_NAMESPACE
class QAbstractItemModel;
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

	// Makes the first (or last) row that has text current, scrolling it into view.
	void focusEdgeRow(bool last);

	// Up / Down / PageUp / PageDown in the message list: the previous / next row with text (a page:
	// about a screen further). LINE moves from a row of its own that setCurrentIndex() does not
	// change, so after Home / End or entering the list its arrows continued from an old position.
	// Returns false for other keys.
	bool stepRow(int key);

	// Chat list cursor. LINE opens a chat whenever the current row changes and stalls while it
	// loads (up to ~2 s per chat), so Up/Down/PageUp/PageDown/Home/End move the bridge's own
	// cursor instead; Enter opens the chat under it. Returns false for other keys.
	bool moveCursor(int key);
	// Opens the chat under the cursor if it is not the open one; false if there is nothing to open.
	bool openCursorRow();
	// Back to following LINE's current row (the open chat).
	void resetCursor();
	// The row the user is on: the cursor while it is set, else LINE's current row (-1: none).
	int cursorRow() const;
	bool hasCursor() const { return m_cursor.isValid() && m_cursor.model() == m_model.data(); }

private:
	void connectView();
	void connectModel();
	// Reconnects when the view got a new model.
	void checkModel() const;
	// Drops items past the end once the model has settled after rows went away.
	void schedulePrune() const;
	// Makes `row` current, or reads it again if it already is (NVDA ignores a repeated focus event).
	void makeCurrent(int row);

	mutable QHash<int, QAccessible::Id> m_items;
	struct PaintedText {
		QString text;
		qint64 at = 0;
		QPointer<QObject> object; // the row's data object the text belongs to
	};
	mutable QHash<quintptr, PaintedText> m_painted;
	mutable bool m_prunePending = false;
	QList<QMetaObject::Connection> m_connections;
	QList<QMetaObject::Connection> m_modelConnections;
	QPointer<QAbstractItemModel> m_model;
	// Follows its chat when LINE re-sorts the list (new messages move chats to the top).
	QPersistentModelIndex m_cursor;
};

} // namespace line_a11y
