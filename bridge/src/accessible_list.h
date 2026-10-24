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

	// Coming back to the messages with Tab / Shift+Tab: makes the row the user last read current
	// again. False (nothing changed) if there is none in this chat (LINE gives each chat a new model).
	bool focusRememberedRow();

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
	// Grids and lists LINE gives no keyboard (a chat's Photos & videos, Links, Files): the arrows,
	// PageUp/PageDown and Home/End move the bridge's cursor by what is on screen (Left/Right one
	// item, Up/Down the nearest item in the line above / below). Returns false for other keys.
	bool moveGridCursor(int key);
	// Clicks the item under the cursor (or the current one), as a mouse user would.
	bool clickCursorRow();
	// Space / Enter on a long message: clicks its "See more" (ดูเพิ่มเติม) link, which shows the
	// whole text. False if the current row has none.
	bool openSeeMore();
	// Space / Enter on a message with a web address (a link preview, a text with a link): opens the
	// first one in the user's default browser (clicking LINE's preview card did nothing). False if
	// the row shows no web address.
	bool openLink();
	// Space / Enter on a voice message, video, file, photo or location: clicks where a mouse user
	// would (play / pause button, video play button, the file's bubble, the picture), so LINE plays
	// or opens it. False for other rows (text, stickers, calls).
	bool openMessageContent();
	// Space / Enter in a picker list (Share with...): ticks or unticks the current row the way a
	// click does (LINE ignores those keys there). False if the row is not a picker row.
	bool toggleCursorRow();
	// Applications key / Shift+F10 on a message: Qt asks for the menu at the centre of the row,
	// which is often beside the bubble (LINE then shows the chat background's menu: Select all /
	// Set wallpaper). Right-clicks the message's bubble instead; if LINE still shows the background's
	// menu (voice messages, files: the first point was not on the part LINE reacts to), closes it
	// and tries the message's other parts. False if no point can be placed.
	bool openRowMenu();
	// Applications key / Shift+F10 in the chat or friend list: Qt would open the menu of LINE's
	// current row (the open chat), not of the row the bridge's cursor is on (the one NVDA read), so
	// "Delete" could hit another chat. Right-clicks the cursor row instead and keeps the cursor
	// there once the menu closes. False if there is no row.
	bool openCursorMenu();
	// Applications key in a chat's Photos & videos / Links / Files window, where LINE has no menu:
	// the bridge's own (Open; Save for photos, videos and files; Copy link and Copy text for links).
	bool openContentMenu();
	// Space / Enter in a chat's search results (ChatSearchResultWindow, the search bar's list
	// button): double-clicks the row like a mouse user (LINE then moves the chat to that message;
	// a single click does nothing) and puts the keyboard on that message in the chat. False in
	// other lists.
	bool openSearchResult();
	// The sender's picture of the received message at `row`: in that row, or (LINE draws the picture
	// and the name only on the first of several messages in a row from one person) in a row above it.
	// Returns that row, or -1 (own messages, notices, dates, or not found). With `scroll`, rows are
	// scrolled into view to be looked at; without, only rows already shown are. Sets `name` to the
	// sender's name drawn above the bubble (groups; empty in one-to-one chats) and `picture` to the
	// picture's place in its row.
	int senderPictureRow(int row, bool scroll, QString *name = nullptr, QRectF *picture = nullptr) const;
	// Clicks the sender's picture of the message at `row` like a mouse user: LINE opens that
	// person's profile. False if the message has none.
	bool openSenderProfile(int row);
	// Hovers the message at `row` and clicks the reaction button LINE then draws beside it (LINE
	// opens its reaction picker). False if the row cannot be looked at.
	// remove: take back the reaction the user gave (press the chosen face).
	bool openReactions(int row, bool remove = false);
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
	// The message row last read while the list had focus (focusRememberedRow()).
	QPersistentModelIndex m_lastRead;
};

// The message menu the keyboard opened (openRowMenu) gets one more item, "Show <sender>'s profile",
// for received messages (user request): a widget of the bridge's own inside LINE's menu, reached and
// pressed with the menu keys the bridge already handles. Called once the menu is shown.
void addProfileMenuItem(QWidget *menu);

// The sticker window's package strip has no names; LINE draws them only in "My stickers" (its Edit
// button). When names are missing or the number of packages changed, open "My stickers", read both
// tabs, go back and put focus back (user request). Called when a StickerWindow shows.
void refreshStickerPackageNames(QWidget *stickerWindow);
// Removes that item again (the menu is hidden; LINE may show the same menu widget again).
void removeExtraMenuItems(QWidget *menu);
bool isExtraMenuItem(const QWidget *w);
// The item's interface (menu item with a press action), nullptr for other widgets.
QAccessibleInterface *extraMenuItemAccessible(QWidget *w);
// An item the bridge asked to press in the next menu LINE shows within a few seconds (a file's
// "Save as..." after jumping to its message), matched by text; nullptr if none is pending or matches.
QWidget *takePendingMenuChoice(const QList<QWidget *> &items);

} // namespace line_a11y
