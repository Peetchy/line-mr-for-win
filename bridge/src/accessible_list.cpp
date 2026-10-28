#include "accessible_list.h"
#include "accessible_widgets.h"

#include "a11y_announce.h"
#include "a11y_event_filter.h"
#include "a11y_log.h"
#include "line_introspect.h"
#include "qt_signal_utils.h"
#include "text_capture.h"

#include <QtCore/QAbstractItemModel>
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QMetaProperty>
#include <QtCore/QPointer>
#include <QtCore/QRegularExpression>
#include <QtCore/QSet>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QClipboard>
#include <QtGui/QEnterEvent>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtWidgets/QScrollBar>

#include <algorithm>
#include <memory>

namespace line_a11y {
namespace {

constexpr int kItemDataRole = Qt::UserRole + 1; // MultipleSectionListModel / LcSimpleListModel "itemData"
constexpr const char *kCurrentIndexSignal = "currentIndexChanged()";
// Painted row texts are reused this long (NVDA asks for a name several times per focus change).
constexpr qint64 kRowTextTtlMs = 1500;

// The bridge interface of `view`, looked up whenever it is needed. Never keep an interface's id
// from its constructor: QAccessible::uniqueId() there registers the interface in Qt's cache before
// Qt does, Qt then registers it a second time, and when the view is destroyed Qt deletes it twice
// (LINE's heap-corruption crashes when a window with a list closed or a photo grid went away).
LineAccessibleList *listFor(const QPointer<QAbstractItemView> &view)
{
	return view ? dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(view.data())) : nullptr;
}

// ---- Row text from the model's data object ------------------------------------------------------

// LINE's list models hand out one QObject per row (ChatData, ...). Most expose the row's content as
// Q_PROPERTYs, which is both cheaper and more reliable than reading the painted row.
int propertyPriority(const QByteArray &name)
{
	static const char *const groups[][8] = {
		{ "name", "displayName", "title", "senderName", "nickname", "userName", "fromName", nullptr },
		{ "memberCount", "count", nullptr },
		{ "lastMessage", "text", "message", "content", "statusMessage", "description", "subtitle", nullptr },
		{ "lastUpdatedTimeForUI", "time", "timeText", "date", "dateText", "createdTime", nullptr },
		{ "unreadCount", "badgeCount", "newCount", nullptr },
	};
	for (int g = 0; g < int(std::size(groups)); ++g) {
		for (const char *const *n = groups[g]; *n; ++n) {
			if (name == *n)
				return g;
		}
	}
	return 2; // unknown text-like properties sit with the content
}

bool isIdentifierProperty(const QByteArray &name)
{
	static const char *const suffixes[] = { "Id", "Source", "Path", "Url", "URL", "Uri", "Image", "Icon", "Color",
	                                        "Key", "WithoutQuery", "Query", "Time" };
	// "mid" is LINE's internal user / group id; "elapsedTime" duplicates lastUpdatedTimeForUI.
	if (name == "id" || name == "mid" || name == "objectName" || name == "section" || name == "sectionId"
	    || name == "type" || name == "status")
		return true;
	if (name == "lastUpdatedTimeForUI")
		return false;
	for (const char *suffix : suffixes) {
		if (name.endsWith(suffix))
			return true;
	}
	return false;
}

QString countPhrase(const QByteArray &name, int value)
{
	if (value <= 0)
		return QString();
	if (name == "memberCount")
		return label("%1 members").arg(value);
	if (name == "count") // SectionData: number of entries under a section header
		return QString::number(value);
	if (name == "unreadCount" || name == "badgeCount" || name == "newCount")
		return label("%1 unread").arg(value > 999 ? QStringLiteral("999+") : QString::number(value));
	return QString();
}

// LINE puts small HTML into last messages (call icons, profile links).
QString cleanMessage(const QString &text)
{
	static const QRegularExpression img(QStringLiteral("<img[^>]*>"));
	QString t = text;
	t.remove(img);
	return cleanText(t);
}

QString dataObjectText(const QObject *data)
{
	if (!data)
		return QString();
	struct Part {
		int priority;
		int order;
		QString text;
	};
	QList<Part> parts;
	const QMetaObject *mo = data->metaObject();
	const int first = QObject::staticMetaObject.propertyCount();
	// Each data class's properties once (names and types; values only for bool / int).
	static QSet<QByteArray> described;
	if (!described.contains(mo->className())) {
		described.insert(mo->className());
		QStringList props;
		for (int i = first; i < mo->propertyCount(); ++i) {
			const QMetaProperty prop = mo->property(i);
			QString entry = QString::fromLatin1(prop.name()) + u':' + QString::fromLatin1(prop.typeName());
			if (prop.metaType().id() == QMetaType::Bool || prop.metaType().id() == QMetaType::Int)
				entry += u'=' + prop.read(data).toString();
			props << entry;
		}
		a11yLog("[list] %s properties: %s", mo->className(), qPrintable(props.join(u' ')));
	}
	for (int i = first; i < mo->propertyCount(); ++i) {
		const QMetaProperty prop = mo->property(i);
		const QByteArray name = prop.name();
		if (isIdentifierProperty(name))
			continue;
		const QVariant v = prop.read(data);
		QString text;
		if (v.metaType().id() == QMetaType::QString)
			text = cleanMessage(v.toString());
		else if (v.metaType().id() == QMetaType::Int)
			text = countPhrase(name, v.toInt());
		if (!text.isEmpty())
			parts.append({ propertyPriority(name), i, text });
	}
	std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) {
		return a.priority != b.priority ? a.priority < b.priority : a.order < b.order;
	});
	QStringList texts;
	for (const Part &p : std::as_const(parts)) {
		if (!texts.contains(p.text))
			texts << p.text;
	}
	return texts.join(QLatin1StringView(", "));
}

// ---- Row text from what LINE paints ------------------------------------------------------------

// Time ("2:49 PM", "14:49", "14.49 น.") and read receipt ("Read", "Read 3", "อ่านแล้ว") labels.
bool isTimeOrReceipt(const QString &text)
{
	static const QRegularExpression pattern(QStringLiteral(
		"^(\\d{1,2}[:.]\\d{2}(\\s?([AaPp][Mm]|น\\.?))?|Read( \\d+)?|อ่านแล้ว( \\d+)?)$"));
	return pattern.match(text.trimmed()).hasMatch();
}

// The menu LINE draws over photos (Save / Save as... / Share / Send to Keep Memo), painted as one
// line of adjacent labels: not part of the message.
bool isPhotoMenu(const QString &text)
{
	static const QStringList labels = {
		QStringLiteral("บันทึกเป็น..."), QStringLiteral("ส่งไปยัง Keep Memo"), QStringLiteral("บันทึก"),
		QStringLiteral("แชร์"), QStringLiteral("Save as..."), QStringLiteral("Send to Keep Memo"),
		QStringLiteral("Save"), QStringLiteral("Share"),
	};
	QString rest = text;
	rest.remove(u' ');
	bool any = false;
	for (bool progress = true; progress && !rest.isEmpty();) {
		progress = false;
		for (const QString &l : labels) {
			const QString compact = QString(l).remove(u' ');
			if (rest.startsWith(compact)) {
				rest = rest.mid(compact.size());
				progress = any = true;
				break;
			}
		}
	}
	return any && rest.isEmpty();
}

bool isReceipt(const QString &text)
{
	static const QRegularExpression pattern(QStringLiteral("^(Read( \\d+)?|อ่านแล้ว( \\d+)?)$"));
	return pattern.match(text.trimmed()).hasMatch();
}

// "ขนาด 2.6MB" / "Size: 388 KB" under a file's name.
bool isFileSize(const QString &text)
{
	static const QRegularExpression pattern(QStringLiteral("^(ขนาด|Size:?)\\s*[\\d.,]+\\s*(B|KB|MB|GB)$"),
	                                        QRegularExpression::CaseInsensitiveOption);
	return pattern.match(text.trimmed()).hasMatch();
}

// Reactions given to a message: small round faces LINE draws just under its bubble or picture.
bool isReactionBadge(const CaptureResult &cap, const QRectF &icon)
{
	if (icon.width() > 26 || icon.height() > 26)
		return false;
	for (const PaintedImage &above : cap.images) {
		const QRectF &b = above.rect;
		if (b != icon && b.width() >= 40 && b.height() >= 30 && icon.top() >= b.bottom() - 6
		    && icon.top() <= b.bottom() + 20 && icon.right() >= b.left() && icon.left() <= b.right())
			return true;
	}
	return false;
}

// The reactions under a message: "Reactions: Like, Funny, 3 people" (LINE draws up to 7 faces, one
// per kind given, left to right, and the number of people; no number per kind), "Like 2 people"
// with one kind, "has reactions" when the faces are not recognised.
QString reactionSummary(const CaptureResult &cap, const QString &count)
{
	QList<const PaintedImage *> badges;
	for (const PaintedImage &pi : cap.images) {
		if (isReactionBadge(cap, pi.rect))
			badges << &pi;
	}
	if (badges.isEmpty())
		return QString();
	std::sort(badges.begin(), badges.end(), [](const PaintedImage *a, const PaintedImage *b) { return a->rect.x() < b->rect.x(); });
	QStringList names;
	for (const PaintedImage *pi : std::as_const(badges)) {
		if (!pi->icon.startsWith(QLatin1StringView("badge-")))
			continue;
		if (const QString name = humanizeIconName(pi->icon); !names.contains(name))
			names << name;
	}
	if (names.isEmpty())
		return label("has reactions");
	if (names.size() == 1)
		return count.isEmpty() ? label("Reaction: %1").arg(names.first()) : label("%1 %2 people").arg(names.first(), count);
	const QString kinds = label("Reactions: %1").arg(names.join(QStringLiteral(", ")));
	return count.isEmpty() ? kinds : label("%1, %2 people").arg(kinds, count);
}

// Logs a small picture once per size and kind (which icons LINE draws where messageKind() looks).
void logIconOnce(const char *what, const PaintedImage &pi)
{
	static QSet<QString> seen;
	const QString key = QStringLiteral("%1 %2x%3 %4").arg(QLatin1StringView(what)).arg(pi.size.width())
	                        .arg(pi.size.height()).arg(pi.icon);
	if (seen.size() > 200 || seen.contains(key))
		return;
	seen.insert(key);
	a11yLog("[kind] %s: %.0fx%.0f image (%dx%d) icon \"%s\"", what, pi.rect.width(), pi.rect.height(),
	        pi.size.width(), pi.size.height(), qUtf8Printable(pi.icon));
}

// What a message without (much) text is, from what LINE draws for it. ChatMessageData has no
// properties (nor dynamic properties or children), the model returns it for every role, and the row's
// delegate (ChatRecvStickerDelegateWidget, ...) paints without Qt events or signals (probed with
// type-probe), so the drawing is all there is (measured on LINE 26.5):
//  - LINE's own icons are recognised by their shape (knownIcon()): a voice message has a play / pause
//    icon in a small bubble and its length ("00:04") drawn there too, a video a play icon on its
//    thumbnail (the two triangles look alike, so what they are drawn on decides), a file a file type
//    icon, a location a red flag, sound stickers and stickers that failed to load an icon of their own;
//  - text bubbles, date pills and notices are images with the text drawn inside them;
//  - a photo is an opaque image drawn under a clip (rounded corners), a video the same with a
//    small play icon at its centre; an album is a grid of equal squares;
//  - a sticker is a large transparent image without a clip (or an empty bubble), LINE emoji are
//    smaller images (~60 px);
//  - a call is a small bubble (~85x76) holding a 24 px phone icon and its length or status;
//  - pictures LINE has not loaded leave only their frame (a large empty rectangle); a video's
//    play button is drawn even then.
// Returns the English label ("Sticker", ...) or nullptr; `lengthBubble` receives the bubble of a
// call or voice message (its length is drawn inside).
const char *messageKind(const CaptureResult &cap, const QRect &rowRect, bool fileSize, QRectF *lengthBubble)
{
	if (fileSize)
		return "File";
	auto textInside = [&cap](const QRectF &r) {
		const QRectF area = r.adjusted(-2, -2, 2, 2);
		for (const CapturedText &it : cap.items) {
			if (!it.text.trimmed().isEmpty() && area.contains(it.pos))
				return true;
		}
		return false;
	};
	// LINE's own icons. The play triangles of videos and voice messages look alike (as do the arrows
	// of "See more" under long texts and of location bubbles, 16 px): one drawn on a large picture (or
	// its empty frame) is a video's, one in a small bubble with a length ("00:04") a voice message's.
	// (The row's own background, as wide as the row, and text bubbles are not pictures.)
	auto isPicture = [&rowRect](const QRectF &r) {
		return r.width() >= 100 && r.height() >= 100 && r.width() < rowRect.width() - 20;
	};
	// (A video's length may be drawn on its thumbnail; thumbnails are clipped or opaque, bubbles not.)
	auto onPicture = [&cap, &isPicture, &textInside](const QRectF &icon) {
		for (const PaintedImage &pi : cap.images) {
			if (isPicture(pi.rect) && pi.rect.contains(icon) && (pi.clipped || !pi.alpha || !textInside(pi.rect)))
				return true;
		}
		for (const PaintedShape &s : cap.shapes) {
			if (isPicture(s.rect) && s.rect.contains(icon) && !textInside(s.rect))
				return true;
		}
		return false;
	};
	auto isAvatar = [](const PaintedImage &pi) {
		return pi.rect.width() >= 30 && pi.rect.width() <= 45 && pi.rect.height() <= 45 && pi.rect.left() < 20;
	};
	// The quoted sender's small picture in a reply sits inside the bubble, beside their name.
	auto inTextBubble = [&cap, &textInside](const QRectF &icon) {
		for (const PaintedImage &bubble : cap.images) {
			if (bubble.rect != icon && bubble.rect.contains(icon) && textInside(bubble.rect))
				return true;
		}
		return false;
	};
	const PaintedImage *voice = nullptr;
	bool fileIcon = false, stickerIcon = false, photoIcon = false, locationIcon = false;
	for (const PaintedImage &icon : cap.images) {
		// Round profile pictures look like the round sticker icons.
		if (icon.icon.isEmpty() || icon.rect.top() < -4 || isAvatar(icon) || isReactionBadge(cap, icon.rect))
			continue;
		if (icon.icon.startsWith(QLatin1StringView("sticker")) && inTextBubble(icon.rect))
			continue;
		const bool play = icon.icon == QLatin1StringView("voice") || icon.icon == QLatin1StringView("video");
		if (play && icon.rect.width() >= 20 && onPicture(icon.rect))
			return "Video";
		if (play)
			voice = voice ? voice : &icon;
		else if (icon.icon == QLatin1StringView("file"))
			fileIcon = true;
		else if (icon.icon.startsWith(QLatin1StringView("sticker")))
			stickerIcon = true;
		else if (icon.icon == QLatin1StringView("photo"))
			photoIcon = true;
		else if (icon.icon == QLatin1StringView("location"))
			locationIcon = true;
	}
	// A location bubble also has an arrow at its right end that looks like a play icon.
	if (locationIcon)
		return "Location";
	if (voice) {
		static const QRegularExpression length(QStringLiteral("^\\d{1,2}:\\d{2}$"));
		for (const PaintedImage &bubble : cap.images) {
			if (bubble.rect.width() <= voice->rect.width() * 2 || bubble.rect.height() >= 80
			    || !bubble.rect.contains(voice->rect))
				continue;
			const QRectF area = bubble.rect.adjusted(-2, -2, 2, 2);
			for (const CapturedText &it : cap.items) {
				if (area.contains(it.pos) && length.match(it.text.trimmed()).hasMatch()) {
					*lengthBubble = bubble.rect;
					return "Voice message";
				}
			}
		}
	}
	if (fileIcon)
		return "File";
	if (stickerIcon)
		return "Sticker";
	if (photoIcon)
		return "Photo";
	// Pictures cut off at the top belong to the row above (or show only their lower part).
	auto inRow = [](const QRectF &r) { return r.top() >= -4; };
	auto centredIcon = [&cap](const QRectF &r, int minSide, int maxSide) {
		for (const PaintedImage &icon : cap.images) {
			const QRectF ir = icon.rect;
			if (icon.alpha && ir.width() >= minSide && ir.width() <= maxSide && qAbs(ir.width() - ir.height()) < 3
			    && (ir.center() - r.center()).manhattanLength() < 12)
				return true;
		}
		return false;
	};
	int photos = 0, emoji = 0;
	bool video = false, sticker = false;
	for (const PaintedImage &pi : cap.images) {
		const QRectF &r = pi.rect;
		if (isAvatar(pi) || !inRow(r))
			continue;
		// A call: a small bubble with a phone icon inside (and its length or status as text). Only
		// LINE's call icons count: link previews have small bubbles with other icons of that size.
		if (r.height() >= 60 && r.height() <= 95 && r.width() <= 140) {
			for (const PaintedImage &icon : cap.images) {
				if (!icon.alpha || icon.rect.width() < 20 || icon.rect.width() > 28 || !r.contains(icon.rect))
					continue;
				if (!icon.icon.endsWith(QLatin1StringView("call"))) { // also the header's chatroom-call
					logIconOnce("not a call", icon);
					continue;
				}
				*lengthBubble = r;
				return "Call message";
			}
		}
		if (textInside(r))
			continue;
		// A bubble with nothing written in it is a sticker LINE draws inside a bubble (seen at 150x38).
		if (r.width() >= 100 && r.height() >= 30 && r.height() < 60 && pi.alpha && pi.clearCorners && !pi.clipped) {
			sticker = true;
			continue;
		}
		if (r.width() < 48 || r.height() < 48)
			continue;
		const bool large = r.width() >= 100 || r.height() >= 100;
		if (large && (pi.clipped || !pi.alpha)) {
			++photos;
			video = video || centredIcon(r, 24, 48);
		} else if (large && pi.clearCorners) {
			sticker = true;
		} else if (pi.clearCorners && !pi.clipped) {
			++emoji;
		}
	}
	if (video)
		return "Video";
	if (photos > 1)
		return "Photos";
	if (photos == 1)
		return "Photo";
	if (sticker)
		return "Sticker";
	if (emoji > 0)
		return "LINE emoji";
	// Not loaded yet: empty frames. Several equal squares are an album; a video still has its play
	// button (a local icon) at the centre of the frame.
	int squares = 0;
	bool frame = false;
	bool playButton = false;
	for (const PaintedShape &s : cap.shapes) {
		const QRectF &r = s.rect;
		if (!inRow(r) || r.width() >= rowRect.width() - 20 || textInside(r))
			continue;
		if (r.width() >= 80 && r.width() <= 130 && qAbs(r.width() - r.height()) < 2) {
			++squares;
		} else if (r.width() >= 120 && r.height() >= 120) {
			frame = true;
			for (const PaintedShape &icon : cap.shapes) {
				const QRectF &ir = icon.rect;
				if (ir.width() >= 24 && ir.width() <= 48 && qAbs(ir.width() - ir.height()) < 3
				    && (ir.center() - r.center()).manhattanLength() < 12)
					playButton = true;
			}
			playButton = playButton || centredIcon(r, 24, 48);
		}
	}
	if (squares >= 4) // each tile is drawn twice
		return "Photos";
	if (playButton)
		return "Video";
	return frame ? "Photo" : nullptr;
}

// A row of a chat's search results (ChatSearchResultWindow): the sender and the date on the first
// line (the date drawn at the right edge), the matching text below, wrapped. Read as "sender, date,
// text".
QString searchResultRowText(QWidget *viewport, const QRect &rowRect, bool *skipped)
{
	const CaptureResult cap = captureRegionText(viewport, rowRect);
	*skipped = cap.skipped;
	if (cap.skipped)
		return QString();
	struct Part {
		qreal y;
		qreal x;
		qreal right;
		QString text;
	};
	QList<Part> parts;
	for (const CapturedText &it : cap.items) {
		if (it.text.trimmed().isEmpty())
			continue;
		parts.append({ it.pos.y(), it.pos.x(), it.pos.x() + it.width, it.text });
	}
	// Lines: baselines a few pixels apart are one line (the date's font is smaller than the name's).
	std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) { return a.y < b.y; });
	qreal lineY = -1000;
	for (Part &p : parts) {
		if (p.y - lineY > 4)
			lineY = p.y;
		p.y = lineY;
	}
	std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) {
		return a.y != b.y ? a.y < b.y : a.x < b.x;
	});
	// Runs LINE draws separately but touching are one text.
	QList<Part> merged;
	for (const Part &p : std::as_const(parts)) {
		if (!merged.isEmpty() && merged.last().y == p.y && p.x - merged.last().right < 6) {
			merged.last().text += p.text;
			merged.last().right = p.right;
		} else {
			merged.append(p);
		}
	}
	auto isThai = [](QChar c) { return c.unicode() >= 0x0E00 && c.unicode() <= 0x0E7F; };
	QString out;
	for (qsizetype i = 0; i < merged.size(); ++i) {
		const QString t = cleanText(merged[i].text);
		if (t.isEmpty())
			continue;
		if (out.isEmpty()) {
			out = t;
			continue;
		}
		const bool header = merged[i].y == merged.first().y || merged[i - 1].y == merged.first().y;
		if (header)
			out += QStringLiteral(", ") + t;
		else if (isThai(out.back()) && isThai(t.front()))
			out += t; // Thai wraps anywhere
		else
			out += QLatin1Char(' ') + t;
	}
	return out;
}

// A post's photos and videos (Notes of a group / OpenChat: TimelineListView rows drawn by
// PostContentDelegateWidget): pictures of at least 100x80, duplicates (placeholder + picture) once.
// `videos` tells which ones show a play button (album_note/play_icon.svg, 20x20 at the bottom left).
QList<QRectF> postMedia(const CaptureResult &cap, QList<bool> *videos)
{
	QList<QRectF> media;
	for (const PaintedImage &pi : cap.images) {
		if (pi.rect.width() < 100 || pi.rect.height() < 80)
			continue;
		bool seen = false;
		for (const QRectF &m : std::as_const(media))
			seen = seen || m.intersected(pi.rect).width() * m.intersected(pi.rect).height() > 0.8 * m.width() * m.height();
		if (!seen)
			media << pi.rect;
	}
	if (videos) {
		videos->clear();
		for (const QRectF &m : std::as_const(media)) {
			bool video = false;
			for (const PaintedImage &pi : cap.images) {
				if (!m.contains(pi.rect.center()) || pi.rect.width() > 48 || pi.rect.height() > 48)
					continue;
				video = video || pi.icon == QLatin1StringView("video")
				    || (pi.alpha && pi.rect.width() >= 14 && pi.rect.height() >= 14 && pi.rect.center().x() < m.center().x()
				        && pi.rect.center().y() > m.center().y());
			}
			videos->append(video);
		}
	}
	return media;
}

// The post's own text (what Copy text copies): the lines under the author's name and above the
// reaction / comment bar, outside the pictures; lines on one baseline joined, others on new lines.
QString postBodyText(const CaptureResult &cap, const QRect &rowRect)
{
	qreal barTop = rowRect.height() - 50;
	for (const PaintedImage &pi : cap.images) {
		if (pi.icon.startsWith(QLatin1StringView("note-")))
			barTop = qMin(barTop, pi.rect.top() - 2);
	}
	const QList<QRectF> media = postMedia(cap, nullptr);
	QList<const CapturedText *> lines;
	for (const CapturedText &it : cap.items) {
		if (cleanText(it.text).isEmpty() || it.pos.y() < 40 || it.pos.y() > barTop)
			continue;
		bool inMedia = false;
		for (const QRectF &m : media)
			inMedia = inMedia || m.contains(it.pos);
		if (!inMedia)
			lines << &it;
	}
	std::stable_sort(lines.begin(), lines.end(), [](const CapturedText *a, const CapturedText *b) {
		return qAbs(a->pos.y() - b->pos.y()) > 4 ? a->pos.y() < b->pos.y() : a->pos.x() < b->pos.x();
	});
	QString out;
	qreal lastY = -1000;
	for (const CapturedText *it : std::as_const(lines)) {
		if (out.isEmpty())
			out = it->text.trimmed();
		else if (qAbs(it->pos.y() - lastY) <= 4)
			out += it->text; // runs on one line
		else
			out += QLatin1Char('\n') + it->text.trimmed();
		lastY = it->pos.y();
	}
	return out.trimmed();
}

// The "More options" button of a post (album_note/more_icon.svg, top right), from the hidden row
// template LINE draws the posts with: its centre relative to the row.
QPointF postMoreButtonPoint()
{
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (!inheritsClass(top, "PostContentDelegateWidget"))
			continue;
		for (QWidget *child : top->findChildren<QWidget *>()) {
			if (inheritsClass(child, "LcButton") && iconSource(child).contains(QLatin1StringView("album_note/more_icon")))
				return QPointF(child->mapTo(top, QPoint())) + QPointF(child->width() / 2.0, child->height() / 2.0);
		}
	}
	return QPointF();
}

// A post: author, text, its photos / videos, the numbers of reactions and comments, the time
// ("Peet, Video, 2 comments, 8 minutes ago").
QString postRowText(QWidget *viewport, const QRect &rowRect, bool *skipped)
{
	const CaptureResult cap = captureRegionText(viewport, rowRect);
	*skipped = cap.skipped;
	if (cap.skipped)
		return QString();
	struct Part {
		qreal y;
		qreal x;
		qreal right;
		QString text;
	};
	QList<Part> parts;
	for (const CapturedText &it : cap.items) {
		if (!cleanText(it.text).isEmpty())
			parts.append({ it.pos.y(), it.pos.x(), it.pos.x() + it.width, it.text });
	}
	// The numbers beside the reaction / comment buttons say what they count.
	static const QRegularExpression number(QStringLiteral("^\\d[\\d,.]*[KkMm+]?$"));
	for (Part &p : parts) {
		if (!number.match(cleanText(p.text)).hasMatch())
			continue;
		for (const PaintedImage &pi : cap.images) {
			if (!pi.icon.startsWith(QLatin1StringView("note-")) || p.x < pi.rect.right() - 2 || p.x > pi.rect.right() + 12
			    || p.y < pi.rect.top() || p.y > pi.rect.bottom() + 6)
				continue;
			p.text = label(pi.icon == QLatin1StringView("note-comments") ? "%1 comments" : "%1 reactions").arg(cleanText(p.text));
			break;
		}
	}
	QList<bool> videos;
	const QList<QRectF> media = postMedia(cap, &videos);
	if (!media.isEmpty()) {
		const qsizetype videoCount = videos.count(true);
		const qsizetype photoCount = media.size() - videoCount;
		QStringList kinds;
		if (media.size() == 1) {
			kinds << label(videoCount ? "Video" : "Photo");
		} else {
			if (photoCount)
				kinds << label("%1 photos").arg(photoCount);
			if (videoCount)
				kinds << label("%1 videos").arg(videoCount);
		}
		qreal top = media.first().top();
		for (const QRectF &m : media)
			top = qMin(top, m.top());
		parts.append({ top, 0, 0, kinds.join(QStringLiteral(", ")) });
	}
	// Lines: baselines a few pixels apart are one line.
	std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) { return a.y < b.y; });
	qreal lineY = -1000;
	for (Part &p : parts) {
		if (p.y - lineY > 4)
			lineY = p.y;
		p.y = lineY;
	}
	std::stable_sort(parts.begin(), parts.end(), [](const Part &a, const Part &b) {
		return a.y != b.y ? a.y < b.y : a.x < b.x;
	});
	auto isThai = [](QChar c) { return c.unicode() >= 0x0E00 && c.unicode() <= 0x0E7F; };
	QString out;
	const Part *last = nullptr;
	for (const Part &p : std::as_const(parts)) {
		const QString t = cleanText(p.text);
		if (t.isEmpty())
			continue;
		if (!last) {
			out = t;
		} else if (last->y == p.y && p.x - last->right < 6 && p.right > 0) {
			out += t; // runs LINE draws separately
		} else if (last->y != p.y && qAbs(last->x - p.x) <= 2 && p.right > 0 && last->right > 0
		           && p.y - last->y < 30 && p.y > 40) {
			// A wrapped line of the post's text (Thai wraps anywhere).
			out += isThai(out.back()) && isThai(t.front()) ? t : QLatin1Char(' ') + t;
		} else {
			out += QStringLiteral(", ") + t;
		}
		last = &p;
	}
	return out;
}

// A chat message row: sender, content, then time and read receipt (which LINE draws beside the
// bubble). Sent messages are drawn on the right and have no sender name.
QString paintedRowText(QWidget *viewport, const QRect &rowRect, bool messages, bool *skipped)
{
	const CaptureResult cap = captureRegionText(viewport, rowRect);
	*skipped = cap.skipped;
	if (cap.skipped)
		return QString();
	if (!messages) {
		const QString text = cleanText(capturedTextInReadingOrder(cap));
		return text;
	}
	struct Line {
		qreal y;
		qreal x;
		QString text;
		qreal right;
	};
	QList<Line> lines;
	for (const CapturedText &it : cap.items) {
		const QString t = it.text.trimmed();
		if (t.isEmpty())
			continue;
		// Fragments on one baseline that touch belong together (LINE draws runs separately).
		if (!lines.isEmpty() && qAbs(lines.last().y - it.pos.y()) < 1) {
			lines.last().text += it.text;
			lines.last().right = qMax(lines.last().right, it.pos.x() + it.width);
		} else {
			lines.append({ it.pos.y(), it.pos.x(), it.text, it.pos.x() + it.width });
		}
	}
	std::stable_sort(lines.begin(), lines.end(), [](const Line &a, const Line &b) {
		return qAbs(a.y - b.y) > 4 ? a.y < b.y : a.x < b.x;
	});
	// Lines at the same indent are one bubble (joined with a space, or directly between Thai
	// characters, which LINE wraps anywhere); a change of indent (sender name above the bubble,
	// a reply quote) starts a new part.
	auto isThai = [](QChar c) { return c.unicode() >= 0x0E00 && c.unicode() <= 0x0E7F; };
	QStringList content, trailing;
	QList<qreal> trailingX;
	bool receipt = false;
	qreal minX = rowRect.width();
	qreal maxRight = 0;
	qreal lastX = -1000;
	bool photoMenu = false;
	bool fileSize = false;
	// Received group messages start with the sender's name, beside the avatar at the top.
	bool sender = false;
	// The number of people who reacted, drawn right of the reaction faces under the bubble.
	QString reactionCount;
	QRectF faces;
	for (const PaintedImage &pi : cap.images) {
		if (pi.icon.startsWith(QLatin1StringView("badge-")) && isReactionBadge(cap, pi.rect))
			faces = faces.united(pi.rect);
	}
	for (const Line &l : std::as_const(lines)) {
		const QString t = cleanText(l.text);
		if (t.isEmpty())
			continue;
		static const QRegularExpression number(QStringLiteral("^\\d[\\d,]*\\+?$"));
		if (!faces.isEmpty() && reactionCount.isEmpty() && l.x >= faces.left() && l.y >= faces.top() - 6
		    && l.y <= faces.bottom() + 6 && number.match(t).hasMatch()) {
			reactionCount = t;
			continue;
		}
		fileSize = fileSize || isFileSize(t);
		if (isTimeOrReceipt(t)) {
			receipt = receipt || isReceipt(t);
			trailing << t;
			trailingX << l.x;
			continue;
		}
		if (isPhotoMenu(t)) {
			photoMenu = true;
			continue;
		}
		if (content.isEmpty())
			sender = l.y < 18 && l.x < rowRect.width() * 0.3;
		minX = qMin(minX, l.x);
		maxRight = qMax(maxRight, l.right);
		if (!content.isEmpty() && qAbs(l.x - lastX) <= 4) {
			content.last() += isThai(content.last().back()) && isThai(t.front()) ? t : QLatin1Char(' ') + t;
		} else {
			content << t;
		}
		lastX = l.x;
	}
	QRectF lengthBubble;
	const char *kind = messageKind(cap, rowRect, fileSize, &lengthBubble);
	if (content.isEmpty() && trailing.isEmpty()) {
		if (kind)
			return label(kind);
		return cap.pixmaps > 0 || photoMenu ? label("Photo or sticker") : QString();
	}
	// Date separators and notices are a single centred line without a time (as are tall pictures
	// whose time is out of view: those have a kind).
	const QString reactions = reactionSummary(cap, reactionCount);
	if (trailing.isEmpty() && !kind) {
		// (While the mouse is over a message LINE draws its reaction button instead of the time.)
		if (!reactions.isEmpty())
			content << reactions;
		return content.join(QLatin1StringView(", "));
	}
	// A call's or voice message's length ("06:26") is drawn inside its bubble; the time it was sent
	// beside it.
	QStringList lengthText;
	for (qsizetype i = trailing.size() - 1; i >= 0 && !lengthBubble.isEmpty(); --i) {
		if (trailingX[i] >= lengthBubble.left() && trailingX[i] < lengthBubble.right()) {
			lengthText.prepend(trailing.takeAt(i));
			trailingX.removeAt(i);
		}
	}
	// Sent messages are drawn on the right. Rows without text (photos, stickers) are placed by
	// their largest picture; small ones (avatars, icons) do not count.
	bool own = receipt;
	if (!own && !content.isEmpty()) {
		// Notices ("X left the group") are centred and have a time too; sent bubbles end at the right.
		const bool centred = qAbs((minX + maxRight) / 2 - rowRect.width() / 2.0) < rowRect.width() * 0.08;
		own = minX > rowRect.width() * 0.35 && !centred;
	} else if (!own) {
		qreal pictureX = -1;
		for (const QRectF &r : cap.pixmapRects) {
			if (r.width() >= 48 && r.height() >= 48)
				pictureX = pictureX < 0 ? r.left() : qMin(pictureX, r.left());
		}
		own = pictureX > rowRect.width() * 0.35;
	}
	// The type comes after the sender's name, before what the message shows (file name, length).
	if (kind) {
		const qsizetype at = sender && !own ? 1 : 0;
		content.insert(at, label(kind));
		for (qsizetype i = 0; i < lengthText.size(); ++i)
			content.insert(at + 1 + i, lengthText[i]);
	}
	else if (content.isEmpty() || (sender && content.size() == 1 && cap.pixmaps > 1))
		content << label("Photo or sticker");
	if (own)
		content.prepend(label("You"));
	if (!reactions.isEmpty())
		content << reactions;
	return (content + trailing).join(QLatin1StringView(", "));
}

// Sticker / emoji package names in package order, read from "My stickers" and kept in
// sticker-packages.txt (kind TAB position TAB name) for the next start.
QStringList &packageNames(bool emoji)
{
	static QStringList lists[2];
	static bool loaded = false;
	if (!loaded) {
		loaded = true;
		QFile file(bridgeDirectory() + QStringLiteral("sticker-packages.txt"));
		if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
			for (const QString &line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
				const QStringList parts = line.split(QLatin1Char('\t'));
				if (parts.size() != 3)
					continue;
				QStringList &list = lists[parts.at(0) == QLatin1StringView("emoji") ? 1 : 0];
				const int at = parts.at(1).toInt();
				if (at < 0 || at > 500)
					continue;
				while (list.size() <= at)
					list << QString();
				list[at] = parts.at(2);
			}
		}
	}
	return lists[emoji ? 1 : 0];
}

void notePackageName(bool emoji, int row, const QString &name, int rows)
{
	QStringList &list = packageNames(emoji);
	if (rows <= 0 || row < 0 || row >= rows || (list.size() == rows && list.at(row) == name))
		return;
	while (list.size() < rows)
		list << QString();
	while (list.size() > rows)
		list.removeLast();
	list[row] = name;
	QString out;
	for (int kind = 0; kind < 2; ++kind) {
		const QStringList &names = packageNames(kind == 1);
		for (qsizetype i = 0; i < names.size(); ++i)
			out += QStringLiteral("%1\t%2\t%3\n").arg(kind == 1 ? QStringLiteral("emoji") : QStringLiteral("sticker")).arg(i).arg(names.at(i));
	}
	QFile file(bridgeDirectory() + QStringLiteral("sticker-packages.txt"));
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
		file.write(out.toUtf8());
	a11yLog("[sticker] %s package %d is \"%s\"", emoji ? "emoji" : "sticker", row + 1, qUtf8Printable(name));
}

// The topmost line LINE draws in a list row (a package's name above its status).
QString firstPaintedLine(QAbstractItemView *view, const QModelIndex &index)
{
	const QRect r = view->visualRect(index);
	if (r.isEmpty() || !view->viewport()->rect().intersects(r))
		return QString();
	const CaptureResult cap = captureRegionText(view->viewport(), r);
	if (cap.skipped || cap.items.isEmpty())
		return QString();
	qreal top = cap.items.first().pos.y();
	for (const CapturedText &it : cap.items)
		top = qMin(top, it.pos.y());
	QList<const CapturedText *> line;
	for (const CapturedText &it : cap.items) {
		if (qAbs(it.pos.y() - top) < 1 && !it.text.trimmed().isEmpty())
			line << &it;
	}
	std::sort(line.begin(), line.end(), [](const CapturedText *a, const CapturedText *b) { return a->pos.x() < b->pos.x(); });
	QString text;
	for (const CapturedText *it : std::as_const(line))
		text += it->text;
	return cleanText(text);
}

QElapsedTimer &clock()
{
	static QElapsedTimer t;
	if (!t.isValid())
		t.start();
	return t;
}

QString listContextName(const QWidget *view)
{
	struct Context {
		const char *ancestor;
		const char *name;
	};
	static const Context contexts[] = {
		{ "ChatMessageView", "Messages" },
		{ "MainChatPanel", "Chats" },
		{ "MainFriendPanel", "Friends" },
		{ "MainAddFriendPanel", "Add friends" },
		{ "MainCallPanel", "Calls" },
		{ "ChatSearchResultWindow", "Search results" },
	};
	for (const Context &c : contexts) {
		if (ancestorInheriting(view, c.ancestor))
			return label(c.name);
	}
	return QString();
}

void sendClick(QWidget *target, QPointF local)
{
	QPointer<QWidget> guard(target);
	const QPointF scene = target->mapTo(target->window(), local);
	const QPointF global = target->mapToGlobal(local);
	QEnterEvent enter(local, scene, global);
	QCoreApplication::sendEvent(target, &enter);
	if (!guard)
		return;
	QMouseEvent press(QEvent::MouseButtonPress, local, scene, global, Qt::LeftButton, Qt::LeftButton,
	                  Qt::NoModifier);
	QCoreApplication::sendEvent(target, &press);
	if (!guard)
		return;
	QMouseEvent release(QEvent::MouseButtonRelease, local, scene, global, Qt::LeftButton, Qt::NoButton,
	                    Qt::NoModifier);
	QCoreApplication::sendEvent(target, &release);
}

void sendDoubleClick(QWidget *target, QPointF local)
{
	QPointer<QWidget> guard(target);
	sendClick(target, local);
	if (!guard)
		return;
	const QPointF scene = target->mapTo(target->window(), local);
	const QPointF global = target->mapToGlobal(local);
	QMouseEvent dbl(QEvent::MouseButtonDblClick, local, scene, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(target, &dbl);
	if (!guard)
		return;
	QMouseEvent release(QEvent::MouseButtonRelease, local, scene, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(target, &release);
}

bool contextMenuVisible()
{
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (w->isVisible() && inheritsClass(w, "LcContextMenu"))
			return true;
	}
	return false;
}

// A right click at `local`: hover, press, release and, if LINE opened no menu on those, the mouse
// context menu event.
void sendContextClick(QWidget *target, QPointF local)
{
	QPointer<QWidget> guard(target);
	const QPointF scene = target->mapTo(target->window(), local);
	const QPointF global = target->mapToGlobal(local);
	QEnterEvent enter(local, scene, global);
	QCoreApplication::sendEvent(target, &enter);
	QMouseEvent move(QEvent::MouseMove, local, scene, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	if (guard)
		QCoreApplication::sendEvent(target, &move);
	QMouseEvent press(QEvent::MouseButtonPress, local, scene, global, Qt::RightButton, Qt::RightButton,
	                  Qt::NoModifier);
	if (guard)
		QCoreApplication::sendEvent(target, &press);
	QMouseEvent release(QEvent::MouseButtonRelease, local, scene, global, Qt::RightButton, Qt::NoButton,
	                    Qt::NoModifier);
	if (guard)
		QCoreApplication::sendEvent(target, &release);
	if (!guard || contextMenuVisible())
		return;
	QContextMenuEvent menu(QContextMenuEvent::Mouse, local.toPoint(), global.toPoint());
	QCoreApplication::sendEvent(target, &menu);
}

// Where a mouse user right-clicks a message (row coordinates): its bubble or picture (an image with
// text inside, else the largest image or shape that is neither the row's background nor the
// avatar), else its first line of text. Null if the row drew nothing of the kind.
QPointF messageContentPoint(const CaptureResult &cap, const QRect &rowRect)
{
	auto textInside = [&cap](const QRectF &r) {
		for (const CapturedText &it : cap.items) {
			if (!it.text.trimmed().isEmpty() && !isTimeOrReceipt(cleanText(it.text)) && r.contains(it.pos))
				return true;
		}
		return false;
	};
	auto candidate = [&rowRect](const QRectF &r) {
		const bool avatar = r.width() >= 30 && r.width() <= 45 && r.height() <= 45 && r.left() < 20;
		return r.top() >= -4 && r.width() >= 16 && r.height() >= 16 && r.width() < rowRect.width() - 20 && !avatar;
	};
	QRectF best;
	bool bestHasText = false;
	auto consider = [&](const QRectF &r) {
		if (!candidate(r))
			return;
		const bool hasText = textInside(r);
		if ((hasText && !bestHasText) || (hasText == bestHasText && r.width() * r.height() > best.width() * best.height())) {
			best = r;
			bestHasText = hasText;
		}
	};
	for (const PaintedImage &pi : cap.images)
		consider(pi.rect);
	for (const PaintedShape &s : cap.shapes)
		consider(s.rect);
	if (!best.isEmpty())
		return best.center();
	for (const CapturedText &it : cap.items) {
		const QString t = cleanText(it.text);
		if (!t.isEmpty() && !isTimeOrReceipt(t))
			return it.pos + QPointF(qMin<qreal>(it.width / 2, 12), -4);
	}
	return QPointF();
}

// Where to right-click a message (row coordinates), best first: messageContentPoint(), then the other
// pictures, bubbles and icons drawn for it (largest first), then its lines of text. LINE shows the
// message's menu only over the part its row reacts to (for voice messages and files that was not
// always the point messageContentPoint() chose), the chat background's menu elsewhere.
QList<QPointF> messageMenuPoints(const CaptureResult &cap, const QRect &rowRect)
{
	QList<QPointF> points;
	auto add = [&points](const QPointF &p) {
		if (p.isNull() || points.size() >= 6)
			return;
		for (const QPointF &q : std::as_const(points)) {
			if ((q - p).manhattanLength() < 8)
				return;
		}
		points << p;
	};
	add(messageContentPoint(cap, rowRect));
	QList<QRectF> parts;
	auto consider = [&parts, &rowRect](const QRectF &r) {
		const bool avatar = r.width() >= 30 && r.width() <= 45 && r.height() <= 45 && r.left() < 20;
		if (r.top() >= -4 && r.width() >= 8 && r.height() >= 8 && r.width() < rowRect.width() - 20 && !avatar)
			parts << r;
	};
	for (const PaintedImage &pi : cap.images)
		consider(pi.rect);
	for (const PaintedShape &sh : cap.shapes)
		consider(sh.rect);
	std::stable_sort(parts.begin(), parts.end(), [](const QRectF &a, const QRectF &b) {
		return a.width() * a.height() > b.width() * b.height();
	});
	for (const QRectF &r : std::as_const(parts))
		add(r.center());
	for (const CapturedText &it : cap.items) {
		if (!cleanText(it.text).isEmpty())
			add(it.pos + QPointF(qMin<qreal>(it.width / 2, 12), -4));
	}
	return points;
}

// Right-clicks `points[i]` of the message at `index` (row coordinates). If LINE shows the chat
// background's menu, the event filter closes it and the next point is tried (setMenuRetry()); if it
// shows no menu at all, the next point too.
void tryRowMenu(QPointer<QAbstractItemView> view, QPersistentModelIndex index, QList<QPointF> points, int i)
{
	if (!view || !index.isValid() || i >= points.size()) {
		setMenuRetry(nullptr);
		return;
	}
	view->scrollTo(index);
	const QRect r = view->visualRect(index);
	// A tall message may reach past the viewport: click inside the part that is shown.
	const QRectF shown = QRectF(r.intersected(view->viewport()->rect())).adjusted(1, 1, -1, -1);
	if (shown.isEmpty()) {
		setMenuRetry(nullptr);
		return;
	}
	const QPointF local(qBound(shown.left(), r.left() + points[i].x(), shown.right()),
	                    qBound(shown.top(), r.top() + points[i].y(), shown.bottom()));
	static int logged = 0;
	if (logged++ < 30)
		a11yLog("[menu] keyboard menu on message row %d: right click %d/%d at %.0f,%.0f (row %d,%d %dx%d)",
		        index.row(), i + 1, int(points.size()), local.x(), local.y(), r.x(), r.y(), r.width(), r.height());
	auto next = [view, index, points, i] { tryRowMenu(view, index, points, i + 1); };
	setMenuRetry(i + 1 < points.size() ? std::function<void()>(next) : std::function<void()>());
	const int menusBefore = contextMenusShown();
	// LINE may run its menu in a loop of its own: then this returns once the menu has closed.
	sendContextClick(view->viewport(), local);
	if (!view || contextMenusShown() != menusBefore)
		return;
	QTimer::singleShot(150, view, [next, menusBefore, more = i + 1 < points.size()] {
		if (contextMenusShown() != menusBefore)
			return;
		a11yLog("[menu] no menu at that point%s", more ? "; trying another one" : "");
		if (more)
			next();
		else
			setMenuRetry(nullptr);
	});
}

// A sender's round picture beside a received message (row coordinates; LINE also draws it as a
// filled shape with the picture as its brush).
bool isSenderPicture(const QRectF &r)
{
	return r.width() >= 30 && r.width() <= 45 && r.height() >= 30 && r.height() <= 45 && r.left() < 20 && r.top() >= -4
	    && qAbs(r.width() - r.height()) <= 4;
}

QRectF senderPicture(const CaptureResult &cap)
{
	for (const PaintedImage &pi : cap.images) {
		if (isSenderPicture(pi.rect))
			return pi.rect;
	}
	for (const PaintedShape &sh : cap.shapes) {
		if (!sh.texture.isEmpty() && isSenderPicture(sh.rect))
			return sh.rect;
	}
	return QRectF();
}

enum class RowSide { Empty, Received, Other };

// Who a message row is from, by where LINE drew it: received messages start at the left (after the
// sender's picture), the user's own end at the right (often with a read receipt), notices and dates
// are centred.
RowSide rowSide(const CaptureResult &cap, const QRect &rowRect)
{
	const qreal width = rowRect.width();
	qreal minX = width, maxRight = 0, timeRight = -1;
	bool text = false;
	for (const CapturedText &it : cap.items) {
		const QString t = cleanText(it.text);
		if (t.isEmpty() || isPhotoMenu(t))
			continue;
		if (isReceipt(t))
			return RowSide::Other;
		if (isTimeOrReceipt(t)) {
			timeRight = qMax(timeRight, it.pos.x() + it.width);
			continue;
		}
		text = true;
		minX = qMin(minX, it.pos.x());
		maxRight = qMax(maxRight, it.pos.x() + it.width);
	}
	// The time sits left of the user's own bubble (and of a long one that starts near the left),
	// right of a received one.
	if (text && timeRight >= 0 && timeRight <= minX + 2)
		return RowSide::Other;
	if (text) {
		const bool centred = qAbs((minX + maxRight) / 2 - width / 2) < width * 0.08;
		return !centred && minX <= width * 0.35 ? RowSide::Received : RowSide::Other;
	}
	qreal pictureX = -1;
	for (const QRectF &r : cap.pixmapRects) {
		if (r.width() >= 48 && r.height() >= 48)
			pictureX = pictureX < 0 ? r.left() : qMin(pictureX, r.left());
	}
	if (pictureX < 0)
		return RowSide::Empty;
	return pictureX <= width * 0.35 ? RowSide::Received : RowSide::Other;
}

// The sender's name LINE draws above a received group message, beside the picture: the first line
// near the top that is not inside a bubble. Bubbles are images; LINE also fills a shape behind the
// name itself, so shapes do not count.
QString senderNameAbove(const CaptureResult &cap, const QRect &rowRect, const QRectF &picture)
{
	auto inBubble = [&cap, &rowRect](const QPointF &p) {
		for (const PaintedImage &pi : cap.images)
			if (pi.rect.width() < rowRect.width() - 20 && pi.rect.width() >= 16 && pi.rect.contains(p))
				return true;
		return false;
	};
	for (const CapturedText &it : cap.items) {
		const QString t = cleanText(it.text);
		if (t.isEmpty() || isTimeOrReceipt(t) || it.pos.x() < picture.right() || it.pos.y() > picture.bottom()
		    || it.pos.x() > rowRect.width() * 0.5 || inBubble(it.pos))
			continue;
		return t;
	}
	return QString();
}

struct ProfileOffer {
	QPointer<QAbstractItemView> view;
	QPersistentModelIndex index;
	QElapsedTimer at;
	// Judged before the menu opens (while a menu shows, the capture may differ).
	RowSide side = RowSide::Empty;
	bool reactable = false; // "React" is offered (not for dates, notices, calls)
	bool reacted = false;   // the user has reacted: "Remove reaction" instead
	const char *noReaction = nullptr; // why not, for the log
} g_profileOffer;

constexpr const char *kExtraMenuItemProperty = "_lineA11yExtraMenuItem";

QHash<const QWidget *, std::function<void()>> &extraMenuActions()
{
	static QHash<const QWidget *, std::function<void()>> actions;
	return actions;
}

// The bridge's own menu item: invisible (1 px, paints nothing), present for the keyboard and screen
// readers only.
class ExtraMenuItem : public QAccessibleWidget {
public:
	explicit ExtraMenuItem(QWidget *w)
		: QAccessibleWidget(w, QAccessible::MenuItem)
	{
	}
	QString text(QAccessible::Text t) const override
	{
		return t == QAccessible::Name ? widget()->accessibleName() : QString();
	}
	QAccessible::State state() const override
	{
		QAccessible::State st = QAccessibleWidget::state();
		st.focusable = true;
		st.focused = widget()->hasFocus();
		return st;
	}
	QStringList actionNames() const override { return { pressAction() }; }
	void doAction(const QString &action) override
	{
		if (action != pressAction())
			return;
		if (const std::function<void()> press = extraMenuActions().value(widget()))
			QTimer::singleShot(0, widget(), press);
	}
};

// Pickers (ShareWindow "Share with...") tick rows: their data (ChatData, ProfileData) has `checked`.
// The main window's lists carry the same property, always false; section headers have it too.
bool isPickerRow(const QAbstractItemView *view, const QObject *data, bool *checked = nullptr)
{
	// Messages have `checked` too (for selecting several), but a list of messages (search results)
	// is no picker.
	if (!view || !data || data->metaObject()->indexOfProperty("checked") < 0 || data->property("section").toBool()
	    || inheritsClass(data, "ChatMessageData") || inheritsAnyClass(view->window(), { "AllInOneWindow", "ChatWindow" }))
		return false;
	if (checked)
		*checked = data->property("checked").toBool();
	return true;
}

// A picker row was clicked: tell the screen reader once LINE has ticked it.
void notifyPickerRowLater(QAbstractItemView *view, int row)
{
	QPointer<QAbstractItemView> guard(view);
	QTimer::singleShot(150, view, [guard, row] {
		auto *list = listFor(guard);
		QAccessibleInterface *item = list ? list->itemAt(row) : nullptr;
		if (!item)
			return;
		QAccessible::State changed;
		changed.checked = true;
		QAccessibleStateChangeEvent event(item, changed);
		QAccessible::updateAccessibility(&event);
	});
}

// One row of an LcListView.
class LineAccessibleListItem : public QAccessibleInterface, public QAccessibleActionInterface {
public:
	LineAccessibleListItem(QAbstractItemView *view, int row)
		: m_view(view), m_row(row)
	{
	}

	// The chat list: "Selected" before the name for the chat that is open (LINE's current row; the
	// bridge's cursor is separate; user request: "เลือกอยู่ Apple, ปักหมุด, ..."), and "Pinned" right
	// after the name (LINE draws a pin badge on the picture; ChatData has no property for it).
	QString withChatStatus(const QString &text, QObject *data) const
	{
		QString result = text;
		if (chatPinned(data)) {
			const QString name = cleanText(data->property("name").toString());
			const QString pinned = label("Pinned");
			if (!name.isEmpty() && text.startsWith(name))
				result = name + QStringLiteral(", ") + pinned + text.mid(name.size());
			else
				result = pinned + QStringLiteral(", ") + text;
		}
		if (m_view->currentIndex().row() == m_row && m_view->selectionModel()
		    && m_view->selectionModel()->isSelected(index()))
			result = label("Selected") + u' ' + result;
		return result;
	}

	bool chatPinned(QObject *data) const
	{
		struct Seen {
			bool pinned = false;
			QElapsedTimer age;
		};
		static QHash<QString, Seen> seen;
		const QString id = data->property("id").toString();
		Seen &entry = seen[id];
		const QModelIndex idx = index();
		const QRect r = m_view->visualRect(idx);
		const bool onScreen = !r.isEmpty() && m_view->viewport()->rect().contains(r);
		if (!onScreen || (entry.age.isValid() && entry.age.elapsed() < kRowTextTtlMs))
			return entry.pinned; // off screen: what the row showed last time
		const CaptureResult cap = captureRegionText(m_view->viewport(), r);
		if (cap.skipped)
			return entry.pinned;
		entry.pinned = false;
		QStringList drawn;
		for (const PaintedImage &image : cap.images) {
			drawn << QStringLiteral("%1x%2%3 %4").arg(image.size.width()).arg(image.size.height()).arg(image.icon.isEmpty() ? QString() : u'=' + image.icon, image.average.isValid() ? image.average.name() : QString());
			// The blue badge (pin-badge.svg, #587DE4) on the picture's lower right corner; the same
			// place holds a green badge for OpenChats.
			const QColor c = image.average;
			// (Its shape alone matches round profile pictures.)
			if (c.isValid() && image.size.width() >= 20 && c.blue() > c.red() + 60 && c.blue() > c.green() + 30)
				entry.pinned = true;
		}
		entry.age.start();
		static int logged = 0;
		if (logged++ < 12)
			a11yLog("[list] chat row %d: %s", m_row, qPrintable(drawn.join(u' ')));
		return entry.pinned;
	}

	QModelIndex index() const
	{
		QAbstractItemModel *model = m_view ? m_view->model() : nullptr;
		return model ? model->index(m_row, 0, m_view->rootIndex()) : QModelIndex();
	}

	bool isValid() const override { return index().isValid(); }
	QObject *object() const override { return nullptr; }
	QWindow *window() const override { return m_view ? m_view->window()->windowHandle() : nullptr; }
	QAccessibleInterface *parent() const override { return QAccessible::queryAccessibleInterface(m_view.data()); }
	QAccessibleInterface *child(int) const override { return nullptr; }
	int childCount() const override { return 0; }
	int indexOfChild(const QAccessibleInterface *) const override { return -1; }
	QAccessibleInterface *childAt(int, int) const override { return nullptr; }
	void setText(QAccessible::Text, const QString &) override {}
	QAccessible::Role role() const override
	{
		return isPickerRow(m_view, rowData()) ? QAccessible::CheckBox : QAccessible::ListItem;
	}

	QObject *rowData() const
	{
		const QModelIndex idx = index();
		return idx.isValid() ? idx.data(kItemDataRole).value<QObject *>() : nullptr;
	}

	QString text(QAccessible::Text t) const override
	{
		if (t != QAccessible::Name)
			return QString();
		const QModelIndex idx = index();
		if (!idx.isValid())
			return QString();
		QObject *data = idx.data(kItemDataRole).value<QObject *>();
		if (QString text = dataObjectText(data); !text.isEmpty()) {
			if (inheritsClass(data, "ChatData") && ancestorInheriting(m_view, "MainChatPanel"))
				return withChatStatus(text, data);
			return text;
		}
		// "My stickers" (the sticker window's Edit): LINE draws each package's name there; remembered
		// for the package strip.
		if (inheritsClass(data, "StickerEditPackageData") || inheritsClass(data, "SticonEditPackageData")) {
			const QString text = paintedText(data);
			const QString name = firstPaintedLine(m_view, idx);
			if (name.isEmpty())
				return text;
			notePackageName(inheritsClass(data, "SticonEditPackageData"), m_row, name, m_view->model()->rowCount(m_view->rootIndex()));
			// Name, then its status ("never expires"; Thai lines are joined without a space).
			if (text.startsWith(name) && text.size() > name.size())
				return name + QStringLiteral(", ") + text.mid(name.size()).trimmed();
			return text;
		}
		// The sticker window's package strip: pictures only; the first is the recently used ones. Names
		// come from "My stickers" once it has been shown (same order, without "Recently used").
		if (inheritsClass(data, "StickerPackageData") || inheritsClass(data, "SticonPackageData")) {
			if (m_row == 0)
				return label("Recently used");
			const QStringList &names = packageNames(inheritsClass(data, "SticonPackageData"));
			if (names.size() == m_view->model()->rowCount(m_view->rootIndex()) - 1 && !names.at(m_row - 1).isEmpty())
				return names.at(m_row - 1);
			return label("Package %1").arg(m_row);
		}
		// Plain models (Settings' translation languages) give text even for rows off screen.
		if (QString text = cleanText(idx.data(Qt::DisplayRole).toString()); !text.isEmpty())
			return text;
		return paintedText(data);
	}

	QRect rect() const override
	{
		const QModelIndex idx = index();
		if (!idx.isValid())
			return QRect();
		const QRect r = m_view->visualRect(idx);
		return QRect(m_view->viewport()->mapToGlobal(r.topLeft()), r.size());
	}

	QAccessible::State state() const override
	{
		QAccessible::State st;
		const QModelIndex idx = index();
		if (!idx.isValid()) {
			st.invalid = true;
			return st;
		}
		st.selectable = true;
		st.focusable = true;
		if (bool checked = false; isPickerRow(m_view, rowData(), &checked)) {
			st.checkable = true;
			st.checked = checked;
		}
		auto *list = dynamic_cast<LineAccessibleList *>(parent());
		const int cursor = list ? list->cursorRow() : m_view->currentIndex().row();
		// LINE moves the current row without selecting it; NVDA would say "not selected" on every row.
		if (list && list->hasCursor())
			st.selected = m_row == cursor;
		else
			st.selected = (list && list->isRowSelected(m_row)) || m_view->currentIndex() == idx;
		// NVDA ignores a focus event unless the element still has focus when it looks: count the
		// current row as focused while LINE briefly pulls focus into the chat being opened.
		st.focused = m_row == cursor && (m_view->hasFocus() || isBrowsingList(m_view));
		const QRect r = m_view->visualRect(idx);
		if (r.isEmpty() || !m_view->viewport()->rect().intersects(r))
			st.offscreen = true;
		if (r.height() <= 1 || !m_view->isVisible())
			st.invisible = true;
		return st;
	}

	void *interface_cast(QAccessible::InterfaceType t) override
	{
		if (t == QAccessible::ActionInterface)
			return static_cast<QAccessibleActionInterface *>(this);
		return nullptr;
	}

	// QAccessibleActionInterface
	QStringList actionNames() const override
	{
		if (isPickerRow(m_view, rowData()))
			return { toggleAction(), pressAction(), setFocusAction() };
		return { pressAction(), setFocusAction() };
	}
	QStringList keyBindingsForAction(const QString &) const override { return {}; }
	void doAction(const QString &actionName) override
	{
		if (!isValid())
			return;
		QPointer<QAbstractItemView> view = m_view;
		const QPersistentModelIndex index = this->index();
		if (actionName == setFocusAction()) {
			QMetaObject::invokeMethod(view, [view, index] {
				if (view && index.isValid()) {
					view->setFocus(Qt::OtherFocusReason);
					view->setCurrentIndex(index);
				}
			}, Qt::QueuedConnection);
			return;
		}
		if (actionName != pressAction() && actionName != toggleAction())
			return;
		// LcListView dispatches viewport mouse events to the row's delegate, so a click at the
		// row's centre selects / opens it (opening a chat, folding a section, ticking a picker row).
		const bool picker = isPickerRow(m_view, rowData());
		QMetaObject::invokeMethod(view, [view, index, picker] {
			if (!view || !index.isValid())
				return;
			view->scrollTo(index);
			sendClick(view->viewport(), QRectF(view->visualRect(index)).center());
			if (picker)
				notifyPickerRowLater(view, index.row());
		}, Qt::QueuedConnection);
	}

	int row() const { return m_row; }

private:
	QString paintedText(QObject *data) const
	{
		auto *list = dynamic_cast<LineAccessibleList *>(parent());
		return list ? list->paintedRowText(m_row, data) : QString();
	}

	QPointer<QAbstractItemView> m_view;
	int m_row;
};

} // namespace

bool isLineListView(const QWidget *w)
{
	return w && qobject_cast<const QAbstractItemView *>(w) && inheritsClass(w, "LcListView");
}

bool isPostList(const QWidget *w)
{
	return isLineListView(w) && inheritsClass(w, "TimelineListView");
}

// ---- LineAccessibleList --------------------------------------------------------------------------

LineAccessibleList::LineAccessibleList(QWidget *w)
	: QAccessibleWidget(w, QAccessible::List)
{
	connectView();
}

LineAccessibleList::~LineAccessibleList()
{
	for (const QMetaObject::Connection &c : std::as_const(m_connections))
		QObject::disconnect(c);
	for (const QMetaObject::Connection &c : std::as_const(m_modelConnections))
		QObject::disconnect(c);
	// This destructor runs inside QAccessibleCache::deleteInterface() (the view was destroyed): do not
	// change Qt's cache from here. The row items are invalid once the view is gone; delete them on the
	// next turn of the event loop.
	const QList<QAccessible::Id> items = m_items.values();
	if (!items.isEmpty()) {
		QTimer::singleShot(0, QCoreApplication::instance(), [items] {
			for (QAccessible::Id id : items) {
				if (QAccessible::accessibleInterface(id))
					QAccessible::deleteAccessibleInterface(id);
			}
		});
	}
}

QAbstractItemView *LineAccessibleList::view() const
{
	return qobject_cast<QAbstractItemView *>(object());
}

void LineAccessibleList::connectView()
{
	QAbstractItemView *v = view();
	if (!v)
		return;
	const QPointer<QAbstractItemView> listView = v;
	auto notify = [listView] {
		if (auto *list = listFor(listView))
			list->notifyCurrentRow();
	};
	if (const int sig = signalIndex(v, kCurrentIndexSignal); sig >= 0)
		m_connections << connectRaw(v, sig, v, [notify](void **) { notify(); });
	connectModel();
}

void LineAccessibleList::checkModel() const
{
	QAbstractItemView *v = view();
	if (v && v->model() != m_model.data())
		const_cast<LineAccessibleList *>(this)->connectModel();
}

// LINE gives the view a new model (and selection model) for each chat it opens: follow it, and
// forget what the old model's rows said.
void LineAccessibleList::connectModel()
{
	for (const QMetaObject::Connection &c : std::as_const(m_modelConnections))
		QObject::disconnect(c);
	m_modelConnections.clear();
	m_painted.clear();
	m_cursor = QPersistentModelIndex();
	m_lastRead = QPersistentModelIndex();
	QAbstractItemView *v = view();
	m_model = v ? v->model() : nullptr;
	if (!v)
		return;
	const QPointer<QAbstractItemView> listView = v;
	auto notify = [listView] {
		if (auto *list = listFor(listView))
			list->notifyCurrentRow();
	};
	if (QItemSelectionModel *sm = v->selectionModel())
		m_modelConnections << QObject::connect(sm, &QItemSelectionModel::currentChanged, v, notify);
	if (QAbstractItemModel *model = v->model()) {
		auto prune = [listView] {
			if (auto *list = listFor(listView))
				list->schedulePrune();
		};
		auto forget = [listView] {
			if (auto *list = listFor(listView))
				list->m_painted.clear();
		};
		if (ancestorInheriting(v, "ChatMessageView")) {
			// New messages arrive as rows appended at the end; a chat being opened or scrolled
			// back loads many rows at once (or resets the model) and is not announced.
			auto loaded = std::make_shared<QElapsedTimer>();
			loaded->start();
			m_modelConnections << QObject::connect(model, &QAbstractItemModel::modelReset, v, [loaded] { loaded->start(); });
			m_modelConnections << QObject::connect(model, &QAbstractItemModel::rowsInserted, v,
				[listView, loaded](const QModelIndex &, int first, int last) {
					auto *list = listFor(listView);
					if (!list || last - first > 2 || loaded->elapsed() < 2000 || last != list->rowCount() - 1)
						return;
					QTimer::singleShot(400, list->view(), [listView] {
						auto *list = listFor(listView);
						QAbstractItemView *v = list ? list->view() : nullptr;
						if (!v || !v->isVisible())
							return;
						const int row = list->rowCount() - 1;
						QObject *data = v->model()->index(row, 0, v->rootIndex()).data(kItemDataRole).value<QObject *>();
						const QString text = list->paintedRowText(row, data);
						// The user's own messages were just typed by them.
						if (!text.isEmpty() && !text.startsWith(label("You") + QLatin1Char(',')))
							announce(v, text);
					});
				});
		}
		m_modelConnections << QObject::connect(model, &QAbstractItemModel::rowsRemoved, v, prune)
		              << QObject::connect(model, &QAbstractItemModel::modelReset, v, prune)
		              << QObject::connect(model, &QAbstractItemModel::layoutChanged, v, prune)
		              << QObject::connect(model, &QAbstractItemModel::rowsRemoved, v, forget)
		              << QObject::connect(model, &QAbstractItemModel::modelReset, v, forget)
		              << QObject::connect(model, &QAbstractItemModel::layoutChanged, v, forget);
	}
}

void LineAccessibleList::schedulePrune() const
{
	QAbstractItemView *v = view();
	if (!v || m_prunePending)
		return;
	m_prunePending = true;
	const QPointer<QAbstractItemView> listView = v;
	QTimer::singleShot(0, v, [listView] {
		auto *list = listFor(listView);
		if (!list)
			return;
		list->m_prunePending = false;
		// Items are row positions; only those past the new end go away.
		const int rows = list->rowCount();
		for (auto it = list->m_items.begin(); it != list->m_items.end();) {
			if (it.key() >= rows) {
				if (QAccessible::accessibleInterface(it.value()))
					QAccessible::deleteAccessibleInterface(it.value());
				it = list->m_items.erase(it);
			} else {
				++it;
			}
		}
	});
}

void LineAccessibleList::notifyCurrentRow()
{
	QAbstractItemView *v = view();
	// While the chat list cursor is elsewhere, LINE moving its current row is not the user's doing.
	if (!v || !v->isVisible() || !v->hasFocus() || hasCursor())
		return;
	const QModelIndex current = v->currentIndex();
	if (!current.isValid())
		return;
	m_lastRead = current;
	v->scrollTo(current);
	if (QAccessibleInterface *item = itemAt(current.row())) {
		QAccessibleEvent event(item, QAccessible::Focus);
		QAccessible::updateAccessibility(&event);
	}
}

QString LineAccessibleList::paintedRowText(int row, QObject *data) const
{
	QAbstractItemView *v = view();
	if (!v || !v->model())
		return QString();
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	const quintptr key = data ? quintptr(data) : quintptr(row) + 1;
	const qint64 now = clock().elapsed();
	auto cached = m_painted.constFind(key);
	// A message object freed and another one created at the same address is not the same row.
	if (cached != m_painted.cend() && data && cached->object.data() != data) {
		m_painted.remove(key);
		cached = m_painted.cend();
	}
	if (cached != m_painted.cend() && now - cached->at < kRowTextTtlMs)
		return cached->text;
	const QRect r = v->visualRect(idx);
	if (!v->isVisible() || r.isEmpty() || !v->viewport()->rect().intersects(r))
		return cached != m_painted.cend() ? cached->text : QString(); // last text seen on screen
	const bool messages = ancestorInheriting(v, "ChatMessageView") != nullptr || inheritsClass(data, "ChatMessageData");
	bool skipped = false;
	QString text = inheritsClass(v->window(), "ChatSearchResultWindow")
	    ? searchResultRowText(v->viewport(), r, &skipped)
	    : isPostList(v) ? postRowText(v->viewport(), r, &skipped)
	                    : line_a11y::paintedRowText(v->viewport(), r, messages, &skipped);
	if (skipped) {
		// LINE was painting: answer with what we knew and read the row again shortly; NVDA
		// hears about the real text through a name change.
		const QPointer<QAbstractItemView> listView = v;
		QTimer::singleShot(300, v, [listView, row] {
			auto *list = listFor(listView);
			if (!list || row >= list->rowCount())
				return;
			QAccessibleInterface *item = list->itemAt(row);
			if (!item || item->text(QAccessible::Name).isEmpty())
				return;
			QAccessibleEvent event(item, QAccessible::NameChanged);
			QAccessible::updateAccessibility(&event);
		});
		return cached != m_painted.cend() ? cached->text : QString();
	}
	// The photo / video viewer's thumbnail strip (MediaViewerWindow) draws pictures only: say what each
	// one is (a video's thumbnail has a play icon, perhaps its length).
	// The same for a chat's Photos & videos grid (ContentViewerWindow), except its month headings.
	if (inheritsClass(v->window(), "MediaViewerWindow")
	    || (ancestorInheriting(v, "MediaListPanel") && r.width() < v->viewport()->width() / 2)) {
		const CaptureResult cap = captureRegionText(v->viewport(), r);
		if (!cap.skipped) {
			static const QRegularExpression length(QStringLiteral("^\\d{1,2}:\\d{2}(:\\d{2})?$"));
			bool video = length.match(text.trimmed()).hasMatch();
			for (const PaintedImage &pi : cap.images)
				video = video || pi.icon == QLatin1StringView("video") || pi.icon == QLatin1StringView("voice");
			// The viewer's strip draws a video's mark itself: a 20x22 picture at the thumbnail's bottom
			// left, over a dark gradient (60x76 thumbnails). LINE's chat icons are not in the viewer.
			if (inheritsClass(v->window(), "MediaViewerWindow")) {
				for (const PaintedImage &pi : cap.images) {
					const QRectF &ir = pi.rect;
					video = video || (pi.alpha && ir.width() >= 14 && ir.width() <= 28 && ir.height() >= 14 && ir.height() <= 28
					                  && ir.center().y() > r.height() * 0.5 && ir.center().x() < r.width() * 0.4);
				}
			}
			text = text.isEmpty() ? label(video ? "Video" : "Photo")
			                      : label(video ? "Video" : "Photo") + QStringLiteral(", ") + text;
			// The grid's MediaData holds no sender or date: the month heading above is all there is.
			if (ancestorInheriting(v, "MediaListPanel")) {
				for (int h = row - 1; h >= 0; --h) {
					const QModelIndex hi = v->model()->index(h, 0, v->rootIndex());
					if (v->visualRect(hi).width() < v->viewport()->width() / 2)
						continue;
					if (const QString month = paintedRowText(h, hi.data(kItemDataRole).value<QObject *>()); !month.isEmpty())
						text += QStringLiteral(", ") + month;
					break;
				}
			}
		}
	}
	if (text.isEmpty() && cached != m_painted.cend())
		return cached->text; // row momentarily blank
	// A row partly scrolled out yields part of its text; keep the longer reading.
	if (cached != m_painted.cend() && !v->viewport()->rect().contains(r) && cached->text.size() > text.size())
		text = cached->text;
	if (m_painted.size() > 5000)
		m_painted.clear();
	m_painted.insert(key, { text, now, data });
	return text;
}

void LineAccessibleList::focusLastVisibleRow()
{
	QAbstractItemView *v = view();
	if (!v || !v->model())
		return;
	const QModelIndex current = v->currentIndex();
	if (current.isValid() && v->viewport()->rect().intersects(v->visualRect(current)))
		return;
	// The newest row that says something (the bottom rows can be blank spacers).
	const QRect area = v->viewport()->rect();
	int lastRow = -1;
	for (int y = area.bottom() - 2; y > area.top(); y -= 8) {
		const QModelIndex idx = v->indexAt(QPoint(area.center().x(), y));
		if (!idx.isValid() || idx.row() == lastRow)
			continue;
		lastRow = idx.row();
		QAccessibleInterface *item = itemAt(idx.row());
		if (item && !item->text(QAccessible::Name).isEmpty()) {
			v->setCurrentIndex(idx);
			return;
		}
	}
	// Only the text-less part of a tall row (a picture message) is on screen: scroll to the newest
	// row that says something instead of leaving the oldest row current.
	focusEdgeRow(true);
}

void LineAccessibleList::focusEdgeRow(bool last)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return;
	// Rows LINE just added have no position until the view's delayed layout has run.
	v->doItemsLayout();
	const int step = last ? -1 : 1;
	int target = last ? rows - 1 : 0;
	// The rows at the ends can be blank spacers: take the first one from that end with text.
	for (int row = target, tries = 0; row >= 0 && row < rows && tries < 10; row += step, ++tries) {
		v->scrollTo(v->model()->index(row, 0, v->rootIndex()));
		QAccessibleInterface *item = itemAt(row);
		if (item && !item->text(QAccessible::Name).isEmpty()) {
			target = row;
			break;
		}
	}
	makeCurrent(target);
}

bool LineAccessibleList::focusRememberedRow()
{
	QAbstractItemView *v = view();
	checkModel();
	if (!v || !m_lastRead.isValid() || m_lastRead.model() != m_model.data())
		return false;
	const int row = m_lastRead.row();
	v->scrollTo(m_lastRead);
	QAccessibleInterface *item = itemAt(row);
	if (!item || item->text(QAccessible::Name).isEmpty())
		return false;
	makeCurrent(row);
	return true;
}

bool LineAccessibleList::stepRow(int key)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return false;
	const bool page = key == Qt::Key_PageUp || key == Qt::Key_PageDown;
	int step = 0;
	if (key == Qt::Key_Up || key == Qt::Key_PageUp)
		step = -1;
	else if (key == Qt::Key_Down || key == Qt::Key_PageDown)
		step = 1;
	else
		return false;
	const int current = v->currentIndex().isValid() ? v->currentIndex().row() : -1;
	if (current < 0) {
		focusEdgeRow(true);
		return true;
	}
	const int pageHeight = v->viewport()->height();
	int target = current;
	int travelled = 0;
	for (int row = current + step; row >= 0 && row < rows; row += step) {
		const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
		v->scrollTo(idx);
		// Blank rows (spacers) are skipped.
		if (QAccessibleInterface *item = itemAt(row); item && !item->text(QAccessible::Name).isEmpty()) {
			target = row;
			if (!page || travelled >= pageHeight)
				break;
		}
		travelled += v->visualRect(idx).height();
	}
	// At the top LINE loads older messages once the first row is in view; Up then goes on.
	if (target == current)
		v->scrollTo(v->model()->index(step < 0 ? 0 : rows - 1, 0, v->rootIndex()));
	makeCurrent(target);
	return true;
}

void LineAccessibleList::makeCurrent(int row)
{
	QAbstractItemView *v = view();
	if (!v)
		return;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	if (idx != v->currentIndex()) {
		v->setCurrentIndex(idx); // LINE reports the change (notifyCurrentRow)
		return;
	}
	v->scrollTo(idx);
	if (v->hasFocus())
		m_lastRead = idx;
	if (QAccessibleInterface *item = itemAt(row)) {
		QAccessibleEvent event(item, QAccessible::NameChanged);
		QAccessible::updateAccessibility(&event);
	}
}

QString LineAccessibleList::text(QAccessible::Text t) const
{
	if (t == QAccessible::Name) {
		if (QString name = cleanText(widget()->accessibleName()); !name.isEmpty())
			return name;
		// An empty list shows a text beside it instead ("No files shared in this chat"): say it.
		if (rowCount() == 0 && widget()->parentWidget()) {
			for (QObject *sibling : widget()->parentWidget()->children()) {
				auto *text = qobject_cast<QWidget *>(sibling);
				if (text && text->isVisible() && inheritsClass(text, "LcText"))
					if (const QString empty = cleanText(extractLcTextString(text)); !empty.isEmpty())
						return empty;
			}
		}
		return listContextName(widget());
	}
	return QAccessibleWidget::text(t);
}

QAccessible::State LineAccessibleList::state() const
{
	QAccessible::State st = QAccessibleWidget::state();
	st.focusable = true;
	return st;
}

int LineAccessibleList::childCount() const
{
	return rowCount();
}

QAccessibleInterface *LineAccessibleList::child(int index) const
{
	return itemAt(index);
}

int LineAccessibleList::indexOfChild(const QAccessibleInterface *child) const
{
	const auto *item = dynamic_cast<const LineAccessibleListItem *>(child);
	if (!item || item->parent() != this)
		return -1;
	return item->row();
}

QAccessibleInterface *LineAccessibleList::focusChild() const
{
	QAbstractItemView *v = view();
	if (!v || !v->hasFocus())
		return nullptr;
	const int row = cursorRow();
	return row >= 0 ? itemAt(row) : nullptr;
}

bool LineAccessibleList::moveCursor(int key)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return false;
	int row = cursorRow();
	const int height = v->visualRect(v->model()->index(qMax(row, 0), 0, v->rootIndex())).height();
	const int page = height > 0 ? qMax(1, v->viewport()->height() / height - 1) : 5;
	switch (key) {
	case Qt::Key_Up:
		row = row < 0 ? 0 : row - 1;
		break;
	case Qt::Key_Down:
		row = row < 0 ? 0 : row + 1;
		break;
	case Qt::Key_PageUp:
		row -= page;
		break;
	case Qt::Key_PageDown:
		row = row < 0 ? page : row + page;
		break;
	case Qt::Key_Home:
		row = 0;
		break;
	case Qt::Key_End:
		row = rows - 1;
		break;
	default:
		return false;
	}
	row = qBound(0, row, rows - 1);
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	m_cursor = idx;
	// Scrolling does not change LINE's current row (so no chat is opened).
	v->scrollTo(idx);
	if (QAccessibleInterface *item = itemAt(row)) {
		QAccessibleEvent event(item, QAccessible::Focus);
		QAccessible::updateAccessibility(&event);
	}
	return true;
}

bool LineAccessibleList::moveGridCursor(int key)
{
	QAbstractItemView *v = view();
	const int rows = rowCount();
	if (!v || rows == 0)
		return false;
	const int from = qMax(0, cursorRow());
	auto rectOf = [v](int r) { return v->visualRect(v->model()->index(r, 0, v->rootIndex())); };
	// The nearest item in the next line up or down (items are laid out in model order). From a
	// full-width heading ("October 2026") the line's first item.
	auto nextLine = [&](int start, int step) {
		const QRect a = rectOf(start);
		int lineTop = 0, best = start;
		bool inLine = false;
		qreal bestDistance = 0;
		for (int r = start + step; r >= 0 && r < rows; r += step) {
			const QRect b = rectOf(r);
			if (b.isEmpty())
				continue;
			if (!inLine) {
				if (qAbs(b.center().y() - a.center().y()) < qMax(4, a.height() / 2))
					continue; // same line
				lineTop = b.top();
				inLine = true;
			} else if (b.top() != lineTop) {
				break;
			}
			const bool fromHeading = a.width() > 2 * b.width();
			const qreal distance = fromHeading ? qAbs(b.left() - a.left()) : qAbs(b.center().x() - a.center().x());
			if (best == start || distance < bestDistance) {
				best = r;
				bestDistance = distance;
			}
		}
		return best;
	};
	int row = from;
	switch (key) {
	case Qt::Key_Left:
		row = from - 1;
		break;
	case Qt::Key_Right:
		row = from + 1;
		break;
	case Qt::Key_Up:
	case Qt::Key_Down:
		row = nextLine(from, key == Qt::Key_Down ? 1 : -1);
		break;
	case Qt::Key_PageUp:
	case Qt::Key_PageDown: {
		const int height = qMax(1, rectOf(from).height());
		for (int i = qMax(1, v->viewport()->height() / height); i > 0; --i)
			row = nextLine(row, key == Qt::Key_PageDown ? 1 : -1);
		break;
	}
	case Qt::Key_Home:
		row = 0;
		break;
	case Qt::Key_End:
		row = rows - 1;
		break;
	default:
		return false;
	}
	row = qBound(0, row, rows - 1);
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	m_cursor = idx;
	v->scrollTo(idx);
	if (QAccessibleInterface *item = itemAt(row)) {
		// NVDA ignores a second Focus event for the same item: at either end, repeat the name.
		QAccessibleEvent event(item, row == from ? QAccessible::NameChanged : QAccessible::Focus);
		QAccessible::updateAccessibility(&event);
	}
	return true;
}

bool LineAccessibleList::clickCursorRow()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || row < 0)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx).intersected(v->viewport()->rect());
	if (r.isEmpty())
		return false;
	a11yLog("[keyboard] click on row %d of %s", row, v->window()->metaObject()->className());
	sendClick(v->viewport(), QRectF(r).center());
	return true;
}

bool LineAccessibleList::openCursorRow()
{
	QAbstractItemView *v = view();
	const QPersistentModelIndex idx = m_cursor;
	m_cursor = QPersistentModelIndex();
	if (!v || !idx.isValid() || idx == QPersistentModelIndex(v->currentIndex()))
		return false;
	// A click opens the chat the way LINE expects (setting the current row alone may not).
	v->scrollTo(idx);
	sendClick(v->viewport(), QRectF(v->visualRect(idx)).center());
	return true;
}

bool LineAccessibleList::openSeeMore()
{
	// LINE's link under a long message, in the languages LINE offers.
	static const QStringList kSeeMore = { QStringLiteral("ดูเพิ่มเติม"), QStringLiteral("See more"),
		                                  QStringLiteral("Read more"), QStringLiteral("もっと見る"),
		                                  QStringLiteral("続きを読む"), QStringLiteral("查看更多"),
		                                  QStringLiteral("查看全文"), QStringLiteral("顯示更多"),
		                                  QStringLiteral("查看更多內容"), QStringLiteral("Lihat selengkapnya") };
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const CaptureResult cap = captureRegionText(v->viewport(), r);
	if (cap.skipped)
		return false;
	for (const CapturedText &it : cap.items) {
		if (!kSeeMore.contains(cleanText(it.text)))
			continue;
		// Text positions are baselines: click a little above, in the middle of the word.
		const QPointF inRow(it.pos.x() + qMax<qreal>(it.width, 8) / 2, it.pos.y() - 5);
		QPointF local = QPointF(r.topLeft()) + inRow;
		// A long message is taller than the list: bring its end (where the link is) into view.
		if (!v->viewport()->rect().contains(local.toPoint())) {
			if (QScrollBar *bar = v->verticalScrollBar())
				bar->setValue(bar->value() + int(local.y() - v->viewport()->height() / 2));
			local = QPointF(v->visualRect(idx).topLeft()) + inRow;
		}
		if (!v->viewport()->rect().contains(local.toPoint())) {
			a11yLog("[keyboard] \"See more\" in message row %d is out of view (%.0f,%.0f)", row, local.x(), local.y());
			return false;
		}
		a11yLog("[keyboard] \"See more\" in message row %d: click at %.0f,%.0f", row, local.x(), local.y());
		sendClick(v->viewport(), local);
		return true;
	}
	return false;
}

bool LineAccessibleList::openLink()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const CaptureResult cap = captureRegionText(v->viewport(), r);
	if (cap.skipped)
		return false;
	// The row's lines top to bottom (fragments on one baseline joined), without times and receipts.
	QList<CapturedText> items = cap.items;
	std::stable_sort(items.begin(), items.end(), [](const CapturedText &a, const CapturedText &b) {
		return qAbs(a.pos.y() - b.pos.y()) >= 1 ? a.pos.y() < b.pos.y() : a.pos.x() < b.pos.x();
	});
	QStringList lines;
	qreal lastY = -1e9;
	for (const CapturedText &it : std::as_const(items)) {
		if (it.text.trimmed().isEmpty())
			continue;
		if (!lines.isEmpty() && qAbs(it.pos.y() - lastY) < 1)
			lines.last() += it.text;
		else
			lines << it.text;
		lastY = it.pos.y();
	}
	lines.removeIf([](const QString &l) { return isTimeOrReceipt(cleanText(l)); });
	const QString url = firstWebAddress(lines);
	if (url.isEmpty())
		return false;
	const bool opened = openInBrowser(url);
	// The address may be private: the log names only its host.
	a11yLog("[keyboard] link in message row %d (%s): %s", row, qUtf8Printable(QUrl(url).host()),
	        opened ? "opened in the browser" : "Windows refused to open it");
	return opened;
}

bool LineAccessibleList::openMessageContent()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const CaptureResult cap = captureRegionText(v->viewport(), r);
	if (cap.skipped)
		return false;
	bool fileSize = false;
	for (const CapturedText &it : cap.items)
		fileSize = fileSize || isFileSize(cleanText(it.text));
	QRectF bubble;
	const char *kind = messageKind(cap, r, fileSize, &bubble);
	if (!kind)
		return false;
	auto isPlayIcon = [](const PaintedImage &icon) {
		return icon.icon == QLatin1StringView("voice") || icon.icon == QLatin1StringView("video");
	};
	// Where a mouse user clicks (row coordinates): the play / pause button of a voice message, the
	// play button of a video, else the bubble or picture (a file's bubble opens the file, a photo's
	// picture the viewer).
	QPointF point;
	if (qstrcmp(kind, "Voice message") == 0) {
		for (const PaintedImage &icon : cap.images) {
			if (isPlayIcon(icon) && bubble.contains(icon.rect)) {
				point = icon.rect.center();
				break;
			}
		}
	} else if (qstrcmp(kind, "Video") == 0) {
		for (const PaintedImage &icon : cap.images) {
			if (isPlayIcon(icon) && icon.rect.width() >= 20 && icon.rect.top() >= -4) {
				point = icon.rect.center();
				break;
			}
		}
		if (point.isNull())
			point = messageContentPoint(cap, r);
	} else if (qstrcmp(kind, "File") == 0 || qstrcmp(kind, "Photo") == 0 || qstrcmp(kind, "Photos") == 0
	           || qstrcmp(kind, "Location") == 0) {
		point = messageContentPoint(cap, r);
	} else if (qstrcmp(kind, "Sticker") == 0) {
		// A sound sticker's speaker icon plays its sound again (user request). A click on the sticker
		// itself opens the sticker window at its package: not done.
		QStringList seen;
		for (const PaintedImage &icon : cap.images) {
			seen << QStringLiteral("%1x%2@%3,%4:%5").arg(icon.rect.width()).arg(icon.rect.height()).arg(icon.rect.x())
			            .arg(icon.rect.y()).arg(icon.icon);
			if (icon.icon == QLatin1StringView("sticker-sound") && point.isNull())
				point = icon.rect.center();
		}
		if (point.isNull()) {
			a11yLog("[keyboard] sticker in message row %d has no sound icon (%s)", row, qPrintable(seen.join(QLatin1Char(' '))));
			return false;
		}
	} else {
		return false; // emoji, calls: nothing to open
	}
	if (point.isNull())
		return false;
	// A tall picture may reach past the viewport: click inside the part that is shown.
	const QRectF shown = QRectF(r.intersected(v->viewport()->rect())).adjusted(1, 1, -1, -1);
	if (shown.isEmpty())
		return false;
	const QPointF local(qBound(shown.left(), r.left() + point.x(), shown.right()),
	                    qBound(shown.top(), r.top() + point.y(), shown.bottom()));
	a11yLog("[keyboard] %s in message row %d: click at %.0f,%.0f", kind, row, local.x(), local.y());
	// LINE's row widgets track the mouse: move there first, as a real click does.
	QPointer<QWidget> viewport = v->viewport();
	QMouseEvent move(QEvent::MouseMove, local, viewport->mapTo(viewport->window(), local),
	                 viewport->mapToGlobal(local), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(viewport, &move);
	if (viewport)
		sendClick(viewport, local);
	return true;
}

bool LineAccessibleList::openPostMedia()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0 || !isPostList(v))
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const CaptureResult cap = captureRegionText(v->viewport(), r);
	if (cap.skipped)
		return false;
	QList<bool> videos;
	const QList<QRectF> media = postMedia(cap, &videos);
	if (media.isEmpty()) {
		QStringList seen;
		for (const PaintedImage &pi : cap.images)
			seen << QStringLiteral("%1x%2@%3,%4%5:%6").arg(pi.rect.width()).arg(pi.rect.height()).arg(pi.rect.x())
			            .arg(pi.rect.y()).arg(pi.alpha ? QStringLiteral("a") : QString()).arg(pi.icon);
		a11yLog("[keyboard] post row %d shows no photo or video (%s)", row, qPrintable(seen.join(QLatin1Char(' '))));
		return false;
	}
	// The first one (top, then left), where a mouse user clicks: the middle of the part on screen.
	qsizetype first = 0;
	for (qsizetype i = 1; i < media.size(); ++i) {
		if (media[i].top() < media[first].top() - 4 || (qAbs(media[i].top() - media[first].top()) <= 4 && media[i].left() < media[first].left()))
			first = i;
	}
	const QRectF shown = QRectF(media[first].translated(r.topLeft())).intersected(QRectF(v->viewport()->rect()).adjusted(1, 1, -1, -1));
	if (shown.isEmpty())
		return false;
	const QPointF local = shown.center();
	a11yLog("[keyboard] %s in post row %d: click at %.0f,%.0f", videos[first] ? "video" : "photo", row, local.x(), local.y());
	noteMediaOpenedFrom(v->window());
	QPointer<QWidget> viewport = v->viewport();
	QMouseEvent move(QEvent::MouseMove, local, viewport->mapTo(viewport->window(), local),
	                 viewport->mapToGlobal(local), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(viewport, &move);
	if (viewport)
		sendClick(viewport, local);
	return true;
}

bool LineAccessibleList::toggleCursorRow()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	if (!isPickerRow(v, idx.data(kItemDataRole).value<QObject *>()))
		return false;
	v->scrollTo(idx);
	sendClick(v->viewport(), QRectF(v->visualRect(idx)).center());
	notifyPickerRowLater(v, row);
	return true;
}

bool LineAccessibleList::openSearchResult()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0 || !inheritsClass(v->window(), "ChatSearchResultWindow"))
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	sendDoubleClick(v->viewport(), QRectF(v->visualRect(idx)).center());
	// The chat this window searched: a popped-out chat window titled like it, else the main window.
	auto windowName = [](QWidget *w) {
		QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(w);
		return iface ? iface->text(QAccessible::Name) : QString();
	};
	const QString title = windowName(v->window()); // the chat's name (and member count)
	QWidget *chat = nullptr;
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (!top->isVisible())
			continue;
		const QString name = inheritsClass(top, "ChatWindow") ? windowName(top) : QString();
		if (!name.isEmpty() && !title.isEmpty() && (title.startsWith(name) || name.startsWith(title)))
			chat = top;
		else if (!chat && inheritsClass(top, "AllInOneWindow"))
			chat = top;
	}
	a11yLog("[search] result row %d opened in %s", row, chat ? chat->metaObject()->className() : "-");
	if (!chat)
		return true;
	QPointer<QWidget> guard(chat);
	QTimer::singleShot(400, chat, [guard] {
		if (!guard)
			return;
		for (auto *list : guard->findChildren<QAbstractItemView *>(QStringLiteral("lineMR_list_messages"))) {
			if (!list->isVisible())
				continue;
			guard->activateWindow();
			// Entering the messages while the search bar shows a result starts at the match.
			list->setFocus(Qt::OtherFocusReason);
			break;
		}
	});
	return true;
}

int LineAccessibleList::senderPictureRow(int row, bool scroll, QString *name, QRectF *picture) const
{
	QAbstractItemView *v = view();
	if (!v || !v->model() || row < 0 || !ancestorInheriting(v, "ChatMessageView"))
		return -1;
	const QRect shownArea = v->viewport()->rect();
	// A run of messages from one person rarely has more than a few dozen rows.
	for (int r = row; r >= 0 && r > row - 40; --r) {
		const QModelIndex idx = v->model()->index(r, 0, v->rootIndex());
		if (scroll)
			v->scrollTo(idx);
		const QRect rect = v->visualRect(idx);
		if (!rect.intersects(shownArea))
			return -1;
		const CaptureResult cap = captureRegionText(v->viewport(), rect);
		if (cap.skipped)
			return -1;
		const RowSide side = rowSide(cap, rect);
		if (side == RowSide::Other || (side == RowSide::Empty && r == row))
			return -1;
		if (side == RowSide::Empty)
			continue; // spacer rows between messages
		const QRectF found = senderPicture(cap);
		if (found.isEmpty())
			continue;
		if (name)
			*name = senderNameAbove(cap, rect, found);
		if (picture)
			*picture = found;
		return r;
	}
	return -1;
}

bool LineAccessibleList::openSenderProfile(int row)
{
	QAbstractItemView *v = view();
	QRectF picture;
	const int pictureRow = senderPictureRow(row, true, nullptr, &picture);
	if (!v || pictureRow < 0)
		return false;
	const QModelIndex idx = v->model()->index(pictureRow, 0, v->rootIndex());
	QRect r = v->visualRect(idx);
	QPointF local = r.topLeft() + picture.center();
	// The picture is at the top of its row, which may reach above the viewport.
	if (!v->viewport()->rect().contains(local.toPoint())) {
		v->scrollTo(idx, QAbstractItemView::PositionAtTop);
		r = v->visualRect(idx);
		local = r.topLeft() + picture.center();
	}
	if (!v->viewport()->rect().contains(local.toPoint()))
		return false;
	a11yLog("[keyboard] sender's picture of message row %d (row %d): click at %.0f,%.0f", row, pictureRow, local.x(),
	        local.y());
	QPointer<QWidget> viewport = v->viewport();
	QMouseEvent move(QEvent::MouseMove, local, viewport->mapTo(viewport->window(), local), viewport->mapToGlobal(local),
	                 Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(viewport, &move);
	if (viewport)
		sendClick(viewport, local);
	return true;
}

namespace {

// One of the bridge's items at the bottom of `menu`; `press` runs after the menu has closed.
QWidget *addExtraMenuItem(QWidget *menu, const QString &name, std::function<void()> press)
{
	auto *item = new QWidget(menu);
	item->setProperty(kExtraMenuItemProperty, true);
	item->setAccessibleName(name);
	item->setAttribute(Qt::WA_TransparentForMouseEvents);
	item->setFocusPolicy(Qt::TabFocus);
	// Below LINE's items: the menu keys reach it last (menu items are ordered top to bottom).
	item->setGeometry(0, menu->height() - 1, 1, 1);
	QPointer<QWidget> guard(menu);
	extraMenuActions().insert(item, [guard, press] {
		if (guard)
			guard->hide();
		// After the menu has closed and focus is back on the message.
		QTimer::singleShot(150, qApp, press);
	});
	QObject::connect(item, &QObject::destroyed, [item] { extraMenuActions().remove(item); });
	item->show();
	a11yLog("[menu] added \"%s\"", qUtf8Printable(name));
	return item;
}

} // namespace

namespace {
void sendHover(QWidget *target, QPointF local);
} // namespace

void addProfileMenuItem(QWidget *menu)
{
	removeExtraMenuItems(menu);
	ProfileOffer offer = g_profileOffer;
	g_profileOffer = ProfileOffer();
	if (!menu || !offer.view || !offer.index.isValid() || !offer.at.isValid() || offer.at.elapsed() > 3000)
		return;
	// "React", or "Remove reaction" once the user has reacted (judgeReactions()). LINE draws the
	// (filled) reaction button of the message under the mouse a little later: looked for again while
	// the menu shows, and the item renamed.
	if (offer.reactable) {
		QPointer<QAbstractItemView> view = offer.view;
		QPersistentModelIndex index = offer.index;
		auto remove = std::make_shared<bool>(offer.reacted);
		QPointer<QWidget> item = addExtraMenuItem(menu, label(*remove ? "Remove reaction" : "React"), [view, index, remove] {
			auto *l = view ? dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(view.data())) : nullptr;
			if (!l || !index.isValid() || !l->openReactions(index.row(), *remove))
				a11yLog("[reaction] message row %d could not be looked at", index.isValid() ? index.row() : -1);
		});
		for (int delay : { 100, 250, 500, 900 }) {
			QTimer::singleShot(delay, item, [view, index, remove, item] {
				if (*remove || !view || !index.isValid() || !item || !item->isVisible())
					return;
				const CaptureResult cap = captureRegionText(view->viewport(), view->visualRect(index));
				if (std::none_of(cap.images.cbegin(), cap.images.cend(),
				                 [](const PaintedImage &pi) { return pi.icon == QLatin1StringView("reaction-button-on"); }))
					return;
				*remove = true;
				item->setAccessibleName(label("Remove reaction"));
				a11yLog("[menu] the user has reacted to row %d: \"%s\"", index.row(), qUtf8Printable(item->accessibleName()));
			});
		}
	} else {
		a11yLog("[menu] no reaction item: row %d (%s)", offer.index.row(), offer.noReaction ? offer.noReaction : "empty row");
	}
	// The user's own messages, notices and dates have no sender to show.
	if (offer.side != RowSide::Received) {
		a11yLog("[menu] no profile item: message row %d is not a received message (%d)", offer.index.row(), int(offer.side));
		return;
	}
	auto *list = dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(offer.view.data()));
	if (!list)
		return;
	QString name;
	const int row = offer.index.row();
	const int pictureRow = list->senderPictureRow(row, false, &name);
	if (pictureRow < 0) {
		// The first message of the run (with the picture) may be out of view: look at it on press.
		// Only offered when the message itself is a received one.
		QAbstractItemView *v = offer.view;
		const QRect rect = v->visualRect(offer.index);
		const CaptureResult cap = captureRegionText(v->viewport(), rect);
		if (cap.skipped || rowSide(cap, rect) != RowSide::Received || !chatHasMemberCount(v))
			return;
	}
	// One-to-one chats draw no name above the message, and the chat is the other person's already:
	// no profile item (user request).
	else if (name.isEmpty())
		return;
	QPointer<QAbstractItemView> view = offer.view;
	QPersistentModelIndex index = offer.index;
	addExtraMenuItem(menu, name.isEmpty() ? label("Show the sender's profile") : label("Show %1's profile").arg(name),
	                 [view, index] {
		auto *l = view ? dynamic_cast<LineAccessibleList *>(QAccessible::queryAccessibleInterface(view.data())) : nullptr;
		if (!l || !index.isValid() || !l->openSenderProfile(index.row()))
			a11yLog("[menu] no sender's picture found for message row %d", index.isValid() ? index.row() : -1);
	});
}

void removeExtraMenuItems(QWidget *menu)
{
	if (!menu)
		return;
	for (QWidget *w : menu->findChildren<QWidget *>()) {
		if (isExtraMenuItem(w)) {
			w->hide();
			w->deleteLater();
		}
	}
}

bool isExtraMenuItem(const QWidget *w)
{
	return w && w->property(kExtraMenuItemProperty).toBool();
}

QAccessibleInterface *extraMenuItemAccessible(QWidget *w)
{
	if (!isExtraMenuItem(w))
		return nullptr;
	return new ExtraMenuItem(w);
}

namespace {

// Whether the message menu offers "React" / "Remove reaction" for a row. LINE draws a message's
// reaction button (filled when the user has reacted) while the mouse is over it; dates and notices
// get none, and calls are left out (user request: LINE draws the button beside them too). Without
// the button, a row with a time that is not centred is a message.
void judgeReactions(ProfileOffer &offer, QAbstractItemView *view, const QRect &r, const CaptureResult &first)
{
	// The time (and what the row is) from the row as it is; LINE replaces the time with the
	// button while the mouse is over the message, and draws the button a moment later (see also
	// addProfileMenuItem(), which looks again).
	bool fileSize = false, time = false;
	qreal minX = r.width(), maxRight = 0;
	for (const CapturedText &it : first.items) {
		const QString t = cleanText(it.text);
		if (t.isEmpty())
			continue;
		fileSize = fileSize || isFileSize(t);
		if (isTimeOrReceipt(t)) {
			time = true;
			continue;
		}
		minX = qMin(minX, it.pos.x());
		maxRight = qMax(maxRight, it.pos.x() + it.width);
	}
	QRectF lengthBubble;
	const char *kind = messageKind(first, r, fileSize, &lengthBubble);
	// Notices are centred and start well right of the senders' pictures; a wide received message
	// (a link card) is about centred too, but starts beside the picture.
	const bool centred = maxRight > 0 && minX > r.width() * 0.25 && qAbs((minX + maxRight) / 2 - r.width() / 2.0) < r.width() * 0.08;
	if (const QPointF content = messageContentPoint(first, r); !content.isNull())
		sendHover(view->viewport(), QPointF(r.topLeft()) + content);
	const CaptureResult hovered = captureRegionText(view->viewport(), r);
	bool button = false;
	for (const PaintedImage &pi : (hovered.skipped ? first : hovered).images) {
		offer.reacted = offer.reacted || pi.icon == QLatin1StringView("reaction-button-on");
		button = button || pi.icon.startsWith(QLatin1StringView("reaction-button"));
	}
	if (kind && qstrcmp(kind, "Call message") == 0)
		offer.noReaction = "a call";
	else if (!button && (!time || centred))
		offer.noReaction = "a date or notice";
	offer.reactable = !offer.noReaction;
}

} // namespace

bool LineAccessibleList::openRowMenu()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	g_profileOffer = ProfileOffer();
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const CaptureResult cap = captureRegionText(v->viewport(), r);
	if (cap.skipped)
		return false;
	g_profileOffer.view = v;
	g_profileOffer.index = idx;
	g_profileOffer.side = rowSide(cap, r);
	judgeReactions(g_profileOffer, v, r, cap);
	g_profileOffer.at.start();
	const QList<QPointF> points = messageMenuPoints(cap, r);
	if (points.isEmpty())
		return false;
	tryRowMenu(v, idx, points, 0);
	return true;
}

namespace {

enum class ContentKind { Media, Link, File, Other };

// Which tab of a chat's Photos & videos / Links / Files window (ContentViewerWindow) `v` is.
ContentKind contentKind(const QWidget *v)
{
	if (ancestorInheriting(v, "MediaListPanel"))
		return ContentKind::Media;
	if (ancestorInheriting(v, "LinkListPanel"))
		return ContentKind::Link;
	if (ancestorInheriting(v, "FileListPanel"))
		return ContentKind::File;
	return ContentKind::Other;
}

void sendHover(QWidget *target, QPointF local)
{
	const QPointF scene = target->mapTo(target->window(), local);
	const QPointF global = target->mapToGlobal(local);
	QEnterEvent enter(local, scene, global);
	QCoreApplication::sendEvent(target, &enter);
	QMouseEvent move(QEvent::MouseMove, local, scene, global, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(target, &move);
}

// Ends sendHover(): LINE stops drawing the hovered row's extras (a message's reaction button, which
// replaces its time).
void sendLeave(QWidget *target)
{
	QEvent leave(QEvent::Leave);
	QCoreApplication::sendEvent(target, &leave);
}

// LINE draws a link's copy button and a file's download button only while the mouse is over the row
// (UrlPreviewListDelegateWidget: contents_link_copy_icon.png at the row's right, then "View in chat").
// After hovering: the rightmost small square picture without text in the right half of the row
// (row coordinates), or a null point.
QPointF hoverButtonPoint(const CaptureResult &cap, const QRect &rowRect)
{
	QRectF best;
	for (const PaintedImage &pi : cap.images) {
		const QRectF &r = pi.rect;
		const bool small = r.width() >= 14 && r.width() <= 40 && qAbs(r.width() - r.height()) <= 6;
		if (!small || r.center().x() < rowRect.width() * 0.55)
			continue;
		const bool hasText = std::any_of(cap.items.cbegin(), cap.items.cend(), [&r](const CapturedText &it) {
			return !it.text.trimmed().isEmpty() && r.contains(it.pos);
		});
		if (!hasText && (best.isEmpty() || r.right() > best.right()))
			best = r;
	}
	return best.isEmpty() ? QPointF() : best.center();
}

// The selection bar's buttons under the Photos & videos grid (Cancel, Download, Send to Keep Memo,
// Share; right to left), if shown.
QList<QWidget *> selectionBarButtons(const QWidget *window)
{
	QList<QWidget *> buttons;
	for (QWidget *b : window->findChildren<QWidget *>()) {
		if (b->isVisible() && inheritsClass(b, "LcButton")
		    && b->property("source").toString().contains(QLatin1StringView("common_btn_photo_right")))
			buttons << b;
	}
	std::sort(buttons.begin(), buttons.end(), [](const QWidget *a, const QWidget *b) {
		return a->mapToGlobal(QPoint(0, 0)).x() > b->mapToGlobal(QPoint(0, 0)).x();
	});
	return buttons;
}

bool fileDialogShown()
{
	for (QWidget *w : QApplication::topLevelWidgets()) {
		if (w->isVisible() && inheritsClass(w, "LcFileDialog"))
			return true;
	}
	return false;
}

// Photos & videos: tick the item's check button (MediaListDelegateWidget: 20 px at 154,10 of 189 px),
// press Download in the selection bar that appears (LINE asks for a folder in Windows' dialog), and
// clear the selection once that dialog has closed.
void saveMediaItem(QAbstractItemView *v, const QModelIndex &idx)
{
	const QRect r = v->visualRect(idx);
	const QPointF check(r.right() - 24, r.top() + 20);
	sendHover(v->viewport(), check);
	sendClick(v->viewport(), check);
	QPointer<QWidget> window(v->window());
	QTimer::singleShot(300, window, [window] {
		if (!window)
			return;
		const QList<QWidget *> bar = selectionBarButtons(window);
		static const QStringList kDownload = { QStringLiteral("ดาวน์โหลด"), QStringLiteral("Download"),
			                                   QStringLiteral("ダウンロード"), QStringLiteral("下载"),
			                                   QStringLiteral("下載") };
		QWidget *download = nullptr;
		for (QWidget *b : bar) {
			if (kDownload.contains(cleanText(b->property("text").toString())))
				download = b;
		}
		if (!download && bar.size() >= 2)
			download = bar.at(1);
		if (!download) {
			a11yLog("[content] save: no Download button after ticking the item");
			return;
		}
		a11yLog("[content] save: Download pressed");
		sendClick(download, QRectF(download->rect()).center());
		// After the folder dialog (saved or cancelled), leave selection mode.
		auto *timer = new QTimer(window);
		auto seen = std::make_shared<bool>(false);
		auto started = std::make_shared<QElapsedTimer>();
		started->start();
		QObject::connect(timer, &QTimer::timeout, timer, [window, timer, seen, started] {
			if (fileDialogShown()) {
				*seen = true;
				return;
			}
			if (!*seen && started->elapsed() < 3000)
				return;
			timer->deleteLater();
			if (!window)
				return;
			if (const QList<QWidget *> left = selectionBarButtons(window); !left.isEmpty())
				sendClick(left.first(), QRectF(left.first()->rect()).center());
		});
		timer->start(300);
	});
}

// Links / files: hover the row, then click its button (copy link / download).
void clickHoverButton(QAbstractItemView *v, const QModelIndex &idx, const char *what)
{
	const QRect r = v->visualRect(idx);
	sendHover(v->viewport(), QRectF(r).center());
	QPointer<QAbstractItemView> view(v);
	const QPersistentModelIndex index(idx);
	QTimer::singleShot(150, v, [view, index, what] {
		if (!view || !index.isValid())
			return;
		const QRect rect = view->visualRect(index);
		const CaptureResult cap = captureRegionText(view->viewport(), rect);
		const QPointF point = cap.skipped ? QPointF() : hoverButtonPoint(cap, rect);
		QStringList seen;
		for (const PaintedImage &pi : cap.images)
			seen << QStringLiteral("%1x%2@%3,%4").arg(pi.rect.width()).arg(pi.rect.height()).arg(pi.rect.x()).arg(pi.rect.y());
		for (const PaintedShape &sh : cap.shapes)
			seen << QStringLiteral("shape %1x%2@%3,%4").arg(sh.rect.width()).arg(sh.rect.height()).arg(sh.rect.x()).arg(sh.rect.y());
		for (const CapturedText &it : cap.items)
			seen << QStringLiteral("\"%1\"@%2,%3").arg(it.text.trimmed()).arg(it.pos.x()).arg(it.pos.y());
		a11yLog("[content] %s: %s (pictures %s)", what,
		        point.isNull() ? "no button found"
		                       : qPrintable(QStringLiteral("click at %1,%2").arg(point.x()).arg(point.y())),
		        qPrintable(seen.join(QLatin1Char(' '))));
		if (!point.isNull())
			sendClick(view->viewport(), rect.topLeft() + point);
	});
}

struct PendingMenuChoice {
	QStringList texts;
	QElapsedTimer at;
} g_menuChoice;

// The chat's message list in a window showing one (the main window or a popped-out chat).
QAbstractItemView *visibleMessageList()
{
	for (QWidget *top : QApplication::topLevelWidgets()) {
		if (!top->isVisible())
			continue;
		for (QWidget *w : top->findChildren<QWidget *>()) {
			if (inheritsClass(w, "LcListView") && w->isVisible() && ancestorInheriting(w, "ChatMessageView")
			    && !ancestorInheriting(w, "SearchDetailPanel") && !ancestorInheriting(w, "LcListView"))
				return static_cast<QAbstractItemView *>(w);
		}
	}
	return nullptr;
}

// Files: LINE draws no download button on the row, only "View in chat" (on hover); the message's own
// menu has "Save as...". So: View in chat, then that message's menu, then Save as (LINE asks where).
void saveFileItem(QAbstractItemView *v, const QModelIndex &idx)
{
	const QRect r = v->visualRect(idx);
	sendHover(v->viewport(), QRectF(r).center());
	QPointer<QAbstractItemView> view(v);
	const QPersistentModelIndex index(idx);
	QTimer::singleShot(150, v, [view, index] {
		if (!view || !index.isValid())
			return;
		const QRect rect = view->visualRect(index);
		const CaptureResult cap = captureRegionText(view->viewport(), rect);
		// The text button in the right half of the row ("View in chat", 62x27).
		QRectF button;
		for (const PaintedShape &sh : cap.shapes) {
			const QRectF &b = sh.rect;
			if (b.width() < 40 || b.width() > 160 || b.height() < 18 || b.height() > 40 || b.center().x() < rect.width() * 0.55)
				continue;
			const bool hasText = std::any_of(cap.items.cbegin(), cap.items.cend(), [&b](const CapturedText &it) {
				return !it.text.trimmed().isEmpty() && b.adjusted(-2, -2, 2, 30).contains(it.pos);
			});
			if (hasText)
				button = b;
		}
		if (button.isEmpty()) {
			a11yLog("[content] save file: no View in chat button");
			return;
		}
		a11yLog("[content] save file: View in chat");
		sendClick(view->viewport(), rect.topLeft() + button.center());
		// Once the chat shows the message (LINE makes it the current row), its menu: Save as.
		QTimer::singleShot(1200, qApp, [] {
			QAbstractItemView *messages = visibleMessageList();
			LineAccessibleList *list = messages ? listFor(messages) : nullptr;
			if (!list) {
				a11yLog("[content] save file: no message list after View in chat");
				return;
			}
			g_menuChoice.texts = { QStringLiteral("บันทึกเป็น..."), QStringLiteral("Save as..."), QStringLiteral("Save As..."),
				                   QStringLiteral("名前を付けて保存"), QStringLiteral("另存为"), QStringLiteral("另存新檔") };
			g_menuChoice.at.start();
			messages->window()->activateWindow();
			messages->setFocus(Qt::OtherFocusReason);
			if (!list->openRowMenu())
				a11yLog("[content] save file: no menu for the message");
		});
	});
}

} // namespace

namespace {

// LINE's reaction picker (a ReactionDockWidget in the chat's ChatMessageView): its choices in
// display order (LcImages of cache category "reactionDock").
QList<QWidget *> reactionChoices(QWidget *dock)
{
	QList<QWidget *> choices;
	for (QWidget *w : dock->findChildren<QWidget *>()) {
		if (w->isVisible() && inheritsClass(w, "LcImage") && stringProp(w, "cacheCategory") == QLatin1StringView("reactionDock"))
			choices << w;
	}
	std::sort(choices.begin(), choices.end(), [dock](QWidget *a, QWidget *b) {
		return a->mapTo(dock, QPoint()).x() < b->mapTo(dock, QPoint()).x();
	});
	return choices;
}

// Nothing drawn: every pixel (nearly) transparent.
bool isBlankImage(const QImage &image)
{
	const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
	for (int y = 0; y < argb.height(); ++y) {
		const QRgb *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
		for (int x = 0; x < argb.width(); ++x) {
			if (qAlpha(line[x]) > 16)
				return false;
		}
	}
	return true;
}

QWidget *visibleReactionDock(QWidget *window)
{
	for (QWidget *w : window->findChildren<QWidget *>()) {
		if (w->isVisible() && inheritsClass(w, "ReactionDockWidget"))
			return w;
	}
	return nullptr;
}

// LINE placed the picker where the row template was drawn (far off screen): put it above the
// reaction button `at` (viewport coordinates), make its choices Tab stops and focus the first. Once
// it closes (a choice made, Escape), focus goes back to the message.
void prepareReactionDock(QAbstractItemView *view, QPointF at, bool remove)
{
	QWidget *dock = visibleReactionDock(view->window());
	if (!dock || !dock->parentWidget()) {
		a11yLog("[reaction] the picker did not open");
		return;
	}
	QWidget *parent = dock->parentWidget();
	const QPoint button = view->viewport()->mapTo(parent, at.toPoint());
	const int x = std::clamp(button.x() - dock->width() / 2, 0, qMax(0, parent->width() - dock->width()));
	int y = button.y() - 12 - dock->height();
	if (y < 0)
		y = qMin(button.y() + 12, qMax(0, parent->height() - dock->height()));
	dock->move(x, y);
	dock->raise();
	const QList<QWidget *> choices = reactionChoices(dock);
	QStringList names;
	for (QWidget *choice : choices) {
		choice->setFocusPolicy(Qt::TabFocus);
		choice->setProperty("_lineA11yFocusable", true);
		// The pictures are LINE's reaction faces (text_capture.cpp: "reaction-love", ...).
		const QImage picture = choice->paintingActive() ? QImage() : choice->grab().toImage();
		const QString icon = reactionIcon(picture);
		choice->setProperty("_lineA11yReaction", icon);
		// The faces are animated (16 frames a second): each frame repainted the messages under the
		// picker, and a long message kept LINE so busy that every NVDA query waited ~130 ms (NVDA
		// froze and said nothing). Still faces while the keyboard uses the picker.
		if (choice->property("running").toBool())
			choice->setProperty("running", false);
		// Qt's own interface (a grouping) may be cached from before: now a button.
		repairCachedInterface(choice);
		// LINE leaves the reaction the user already gave blank (pressing it takes it back).
		choice->setProperty("_lineA11yReactionChosen", !picture.isNull() && isBlankImage(picture));
		QString name = icon.startsWith(QLatin1StringView("reaction-")) ? humanizeIconName(icon) : QString();
		// The last, smaller choice (a plain smiley) opens LINE's other reactions.
		if (name.isEmpty() && choice == choices.last() && choice->width() < choices.first()->width())
			name = label("More reactions");
		if (name.isEmpty())
			name = label("Reactions");
		choice->setAccessibleName(name);
		names << QStringLiteral("%1 (%2 %3x%4 clicked()=%5)").arg(name, icon).arg(choice->width()).arg(choice->height())
		                 .arg(int(isSignalConnected(choice, "clicked()")));
	}
	// The reaction already chosen is drawn highlighted and is not recognised. The picker shows LINE's
	// five basic reactions (in an order LINE changes: the one given came first once), so a single
	// unknown face is the kind missing from the others, and it is the chosen one.
	static const char *const kBasic[] = { "reaction-like", "reaction-love", "reaction-fun", "reaction-envy", "reaction-sad" };
	if (choices.size() == 6) {
		int unknown = -1;
		QStringList missing;
		for (const char *kind : kBasic)
			missing << QLatin1String(kind);
		for (int i = 0; i < 5; ++i) {
			const QString icon = choices.at(i)->property("_lineA11yReaction").toString();
			if (icon.isEmpty())
				unknown = unknown < 0 ? i : 99;
			else
				missing.removeAll(icon);
		}
		if (unknown >= 0 && unknown < 5 && missing.size() == 1) {
			choices.at(unknown)->setProperty("_lineA11yReaction", missing.first());
			choices.at(unknown)->setAccessibleName(humanizeIconName(missing.first()));
			names[unknown] = choices.at(unknown)->accessibleName() + QStringLiteral(" (the missing kind)");
			choices.at(unknown)->setProperty("_lineA11yReactionChosen", true);
		}
	}
	for (qsizetype i = 0; i < choices.size(); ++i) {
		QWidget *choice = choices.at(i);
		if (choice->property("_lineA11yReactionChosen").toBool()) {
			choice->setAccessibleName(label("%1, chosen").arg(choice->accessibleName()));
			names[i] = choice->accessibleName();
		}
	}
	a11yLog("[reaction] picker at %d,%d with %d choices: %s", x, y, int(choices.size()), qUtf8Printable(names.join(u", ")));
	if (choices.isEmpty())
		return;
	// "Remove reaction": choosing the reaction given again takes it back.
	if (remove) {
		for (QWidget *choice : choices) {
			if (choice->property("_lineA11yReactionChosen").toBool()) {
				a11yLog("[reaction] removing \"%s\"", qUtf8Printable(choice->accessibleName()));
				sendMouseClick(choice);
				for (QWidget *c : choices)
					c->setProperty("running", true);
				QPointer<QAbstractItemView> list(view);
				QTimer::singleShot(150, view, [list] {
					if (!list)
						return;
					sendLeave(list->viewport());
					list->setFocus(Qt::PopupFocusReason);
				});
				return;
			}
		}
		a11yLog("[reaction] no chosen reaction to remove: the picker stays open");
	}
	for (qsizetype i = 1; i < choices.size(); ++i)
		QWidget::setTabOrder(choices.at(i - 1), choices.at(i));
	// Not a list LINE pulled focus away from: the bridge's list guard must not send it back.
	endListBrowsing();
	choices.first()->setFocus(Qt::OtherFocusReason);
	QPointer<QWidget> guard(dock);
	QPointer<QAbstractItemView> list(view);
	// Qt moves focus on (to the title bar) when the picker hides: remember it was in the picker.
	auto *timer = new QTimer(dock);
	auto started = std::make_shared<QElapsedTimer>();
	started->start();
	auto focused = std::make_shared<bool>(true);
	QObject::connect(timer, &QTimer::timeout, timer, [timer, guard, list, started, focused] {
		if (guard && guard->isVisible() && started->elapsed() < 120000) {
			QWidget *focus = QApplication::focusWidget();
			*focused = focus && guard->isAncestorOf(focus);
			return;
		}
		timer->deleteLater();
		if (guard && guard->isVisible())
			return;
		// Animated again for the mouse.
		if (guard) {
			for (QWidget *choice : guard->findChildren<QWidget *>()) {
				if (choice->property("_lineA11yReaction").isValid() && !choice->property("running").toBool())
					choice->setProperty("running", true);
			}
		}
		if (list)
			sendLeave(list->viewport());
		if (list && list->isVisible() && *focused) {
			a11yLog("[reaction] picker closed: back to the message");
			list->setFocus(Qt::PopupFocusReason);
		}
	});
	timer->start(100);
}

// The reaction button of LINE's hidden row template (a *DelegateWidget top-level) that was just set
// to the row `rect` of `view`: same width, button at the same place. LINE does not always give the
// template the row's height (a 58 px message drawn by a 256 px template): an equal size comes first.
QWidget *reactionTemplateButton(QAbstractItemView *view, const QRect &rect, const QRectF &button)
{
	QWidget *sameWidth = nullptr;
	for (QWidget *w : QApplication::allWidgets()) {
		if (!iconSource(w).contains(QLatin1StringView("ic-reaction-o")) || !inheritsClass(w, "LcButton"))
			continue;
		QWidget *top = w->window();
		if (top == view->window() || top->width() != rect.width() || w->mapTo(top, QPoint()) != button.topLeft().toPoint())
			continue;
		if (top->height() == rect.height())
			return w;
		if (!sameWidth)
			sameWidth = w;
	}
	return sameWidth;
}

QString reactionTemplateCandidates()
{
	QStringList candidates;
	for (QWidget *w : QApplication::allWidgets()) {
		if (!iconSource(w).contains(QLatin1StringView("ic-reaction-o")) || !inheritsClass(w, "LcButton"))
			continue;
		QWidget *top = w->window();
		const QPoint p = w->mapTo(top, QPoint());
		candidates << QStringLiteral("%1 %2x%3 button %4,%5").arg(QLatin1String(top->metaObject()->className()))
		                  .arg(top->width()).arg(top->height()).arg(p.x()).arg(p.y());
	}
	return candidates.join(QStringLiteral("; "));
}

void pressReactionTemplate(QAbstractItemView *view, QWidget *templateButton, QPointF at, bool remove)
{
	QMetaObject::invokeMethod(templateButton, "clicked");
	QPointer<QAbstractItemView> guard(view);
	QTimer::singleShot(80, view, [guard, at, remove] {
		if (guard)
			prepareReactionDock(guard, at, remove);
	});
}

} // namespace

bool LineAccessibleList::openReactions(int row, bool remove)
{
	QAbstractItemView *v = view();
	if (!v || !v->model() || row < 0 || row >= rowCount())
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const CaptureResult cap = captureRegionText(v->viewport(), r);
	if (cap.skipped)
		return false;
	// LINE draws the reaction button beside the message while the mouse is over it.
	QPointF content = messageContentPoint(cap, r);
	if (content.isNull())
		content = QRectF(r).center() - QPointF(r.topLeft());
	sendHover(v->viewport(), QPointF(r.topLeft()) + content);
	QList<QRectF> before;
	for (const PaintedImage &pi : cap.images)
		before << pi.rect;
	QPointer<QAbstractItemView> view(v);
	const QPersistentModelIndex index(idx);
	QTimer::singleShot(200, v, [view, index, before, remove] {
		if (!view || !index.isValid())
			return;
		const QRect rect = view->visualRect(index);
		// Drawing the row also sets LINE's row template (a hidden *DelegateWidget) to this message.
		const CaptureResult hovered = captureRegionText(view->viewport(), rect);
		QRectF button;
		QStringList seen;
		for (const PaintedImage &pi : hovered.images) {
			seen << QStringLiteral("%1x%2@%3,%4%5").arg(pi.rect.width()).arg(pi.rect.height()).arg(pi.rect.x()).arg(pi.rect.y())
			            .arg(pi.icon.isEmpty() ? QString() : QLatin1Char(':') + pi.icon);
			// A small square picture outside every bubble and photo (a thin smiley: its shape alone is
			// too close to a video's play button, which lies inside the thumbnail). One that appeared
			// with the mouse comes first (the menu's right click may have hovered the row already).
			const bool small = pi.rect.width() >= 14 && pi.rect.width() <= 32 && qAbs(pi.rect.width() - pi.rect.height()) <= 3;
			const bool inside = std::any_of(hovered.images.cbegin(), hovered.images.cend(), [&pi](const PaintedImage &o) {
				return o.rect.width() > pi.rect.width() + 4 && o.rect.adjusted(-1, -1, 1, 1).contains(pi.rect);
			});
			if (!small || inside || !pi.alpha)
				continue;
			const bool appeared = std::none_of(before.cbegin(), before.cend(), [&pi](const QRectF &b) {
				return qAbs(b.x() - pi.rect.x()) < 1 && qAbs(b.y() - pi.rect.y()) < 1;
			});
			if (button.isEmpty() || appeared || pi.icon.startsWith(QLatin1StringView("reaction-button")))
				button = pi.rect;
		}
		if (button.isEmpty() || hovered.skipped) {
			a11yLog("[reaction] row %d: no reaction button (pictures %s)", index.row(), qPrintable(seen.join(QLatin1Char(' '))));
			return;
		}
		const QPointF at = QPointF(rect.topLeft()) + button.center();
		// A click on the drawn button does nothing (LINE checks the real mouse); the button of the row
		// template drawing this message opens the picker. LINE may draw the row from its cache, leaving
		// the template set to another message: a data change for the row (the data stays the same)
		// makes the next capture draw it through a template.
		QAbstractItemModel *model = const_cast<QAbstractItemModel *>(index.model());
		Q_EMIT model->dataChanged(QModelIndex(index), QModelIndex(index));
		const QString pictures = seen.join(QLatin1Char(' '));
		QTimer::singleShot(100, view, [view, index, button, at, pictures, remove] {
			if (!view || !index.isValid())
				return;
			const QRect rect = view->visualRect(index);
			const CaptureResult again = captureRegionText(view->viewport(), rect);
			if (QWidget *templateButton = again.skipped ? nullptr : reactionTemplateButton(view, rect, button)) {
				a11yLog("[reaction] row %d: opening the picker (%s %dx%d; pictures %s)", index.row(),
				        templateButton->window()->metaObject()->className(), templateButton->window()->width(),
				        templateButton->window()->height(), qPrintable(pictures));
				pressReactionTemplate(view, templateButton, at, remove);
				return;
			}
			a11yLog("[reaction] row %d: no row template with the button at %.0f,%.0f (%s); clicking", index.row(),
			        button.x(), button.y(),
			        qPrintable(reactionTemplateCandidates()));
			sendHover(view->viewport(), at);
			sendClick(view->viewport(), at);
			QTimer::singleShot(80, view, [view, at, remove] {
				if (view)
					prepareReactionDock(view, at, remove);
			});
		});
	});
	return true;
}

QWidget *takePendingMenuChoice(const QList<QWidget *> &items)
{
	if (g_menuChoice.texts.isEmpty() || !g_menuChoice.at.isValid() || g_menuChoice.at.elapsed() > 5000)
		return nullptr;
	for (QWidget *item : items) {
		QAccessibleInterface *iface = QAccessible::queryAccessibleInterface(item);
		const QString text = iface ? cleanText(iface->text(QAccessible::Name)) : QString();
		if (g_menuChoice.texts.contains(text)) {
			g_menuChoice = PendingMenuChoice();
			a11yLog("[content] pressing \"%s\" in the message's menu", qUtf8Printable(text));
			return item;
		}
	}
	return nullptr;
}

bool LineAccessibleList::openPostMenu()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0 || !isPostList(v))
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const CaptureResult cap = captureRegionText(v->viewport(), r);
	if (cap.skipped || r.isEmpty())
		return false;
	QList<bool> videos;
	const QList<QRectF> media = postMedia(cap, &videos);
	const QString body = postBodyText(cap, r);
	const QPointF more = postMoreButtonPoint();
	if (media.isEmpty() && body.isEmpty() && more.isNull())
		return false;
	const QPersistentModelIndex keep(idx);
	QPointer<QAbstractItemView> view(v);
	auto *menu = new QMenu(v);
	menu->setAttribute(Qt::WA_DeleteOnClose);
	if (!media.isEmpty()) {
		QObject::connect(menu->addAction(label("Open")), &QAction::triggered, v, [view, keep] {
			if (LineAccessibleList *list = view ? listFor(view) : nullptr; list && keep.isValid()) {
				view->setCurrentIndex(keep);
				list->openPostMedia();
			}
		});
		// LINE saves from its viewer only (Download, shown while the mouse is over the picture): open
		// the picture and ask the viewer's bridge to press it (Windows' Save As follows).
		QObject::connect(menu->addAction(label("Save as...")), &QAction::triggered, v, [view, keep] {
			LineAccessibleList *list = view ? listFor(view) : nullptr;
			if (!list || !keep.isValid())
				return;
			QFile request(bridgeDirectory() + QStringLiteral("viewer-save-request"));
			if (request.open(QIODevice::WriteOnly | QIODevice::Truncate))
				request.write(QByteArray::number(QDateTime::currentMSecsSinceEpoch()));
			request.close();
			view->setCurrentIndex(keep);
			if (!list->openPostMedia())
				QFile::remove(request.fileName());
		});
	}
	if (!body.isEmpty()) {
		QObject::connect(menu->addAction(label("Copy text")), &QAction::triggered, v, [body] {
			QGuiApplication::clipboard()->setText(body);
		});
	}
	if (!more.isNull()) {
		QObject::connect(menu->addAction(label("More options")), &QAction::triggered, v, [view, keep, more] {
			if (!view || !keep.isValid())
				return;
			const QRect rr = view->visualRect(keep);
			const QPointF local = QPointF(rr.topLeft()) + more;
			a11yLog("[post] more options of row %d: click at %.0f,%.0f", keep.row(), local.x(), local.y());
			QPointer<QWidget> viewport = view->viewport();
			QMouseEvent move(QEvent::MouseMove, local, viewport->mapTo(viewport->window(), local),
			                 viewport->mapToGlobal(local), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
			QCoreApplication::sendEvent(viewport, &move);
			if (viewport)
				sendClick(viewport, local);
		});
	}
	a11yLog("[post] menu for row %d (%d actions, %d pictures, text %d chars)", row, int(menu->actions().size()),
	        int(media.size()), int(body.size()));
	menu->popup(v->viewport()->mapToGlobal(QRectF(r.intersected(v->viewport()->rect())).center().toPoint()));
	menu->setActiveAction(menu->actions().value(0));
	return true;
}

bool LineAccessibleList::openContentMenu()
{
	QAbstractItemView *v = view();
	if (v && isPostList(v))
		return openPostMenu();
	const int row = cursorRow();
	const ContentKind kind = v ? contentKind(v) : ContentKind::Other;
	if (!v || !v->model() || row < 0 || kind == ContentKind::Other)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	// Month headings span the list.
	if (r.isEmpty() || r.width() > v->viewport()->width() * 0.75)
		return false;
	const QPersistentModelIndex keep(idx);
	QPointer<QAbstractItemView> view(v);
	// The actions' clicks reset the cursor (see the event filter): it stays on the item.
	auto restore = [view, keep] {
		if (LineAccessibleList *list = view ? listFor(view) : nullptr; list && keep.isValid())
			list->m_cursor = keep;
	};
	auto *menu = new QMenu(v);
	menu->setAttribute(Qt::WA_DeleteOnClose);
	QObject::connect(menu->addAction(label("Open")), &QAction::triggered, v, [view, keep, restore] {
		if (view && keep.isValid())
			sendClick(view->viewport(), QRectF(view->visualRect(keep)).center());
		restore();
	});
	if (kind == ContentKind::Media || kind == ContentKind::File) {
		QObject::connect(menu->addAction(label("Save")), &QAction::triggered, v, [view, keep, kind, restore] {
			if (!view || !keep.isValid())
				return;
			if (kind == ContentKind::Media)
				saveMediaItem(view, keep);
			else
				saveFileItem(view, keep);
			restore();
		});
	}
	if (kind == ContentKind::Link) {
		QObject::connect(menu->addAction(label("Copy link")), &QAction::triggered, v, [view, keep, restore] {
			if (view && keep.isValid())
				clickHoverButton(view, keep, "copy link");
			restore();
		});
		QObject::connect(menu->addAction(label("Copy text")), &QAction::triggered, v, [view, keep] {
			LineAccessibleList *list = view ? listFor(view) : nullptr;
			if (QAccessibleInterface *item = list && keep.isValid() ? list->itemAt(keep.row()) : nullptr)
				QGuiApplication::clipboard()->setText(item->text(QAccessible::Name));
		});
	}
	a11yLog("[content] menu for row %d (%d actions)", row, int(menu->actions().size()));
	menu->popup(v->viewport()->mapToGlobal(QRectF(r).center().toPoint()));
	menu->setActiveAction(menu->actions().value(0));
	return true;
}

bool LineAccessibleList::openCursorMenu()
{
	QAbstractItemView *v = view();
	const int row = cursorRow();
	if (!v || !v->model() || row < 0)
		return false;
	const QModelIndex idx = v->model()->index(row, 0, v->rootIndex());
	v->scrollTo(idx);
	const QRect r = v->visualRect(idx);
	const QRectF shown = QRectF(r.intersected(v->viewport()->rect())).adjusted(1, 1, -1, -1);
	if (shown.isEmpty())
		return false;
	const QPersistentModelIndex keep(idx);
	a11yLog("[menu] keyboard menu on list row %d: right click at %.0f,%.0f", row, shown.center().x(),
	        shown.center().y());
	sendContextClick(v->viewport(), shown.center());
	// The click made the cursor follow LINE's current row again (see the event filter); once the
	// menu has closed, go back to the row the menu was for (unless it is gone: deleted, hidden).
	m_cursor = keep;
	QPointer<QAbstractItemView> guard(v);
	auto *timer = new QTimer(v);
	timer->setInterval(200);
	QObject::connect(timer, &QTimer::timeout, timer, [guard, keep, timer] {
		if (!guard || !keep.isValid()) {
			timer->deleteLater();
			return;
		}
		if (contextMenuVisible())
			return;
		timer->deleteLater();
		LineAccessibleList *list = listFor(guard);
		if (!list || !guard->hasFocus())
			return;
		list->m_cursor = keep;
		guard->scrollTo(keep);
		if (QAccessibleInterface *item = list->itemAt(keep.row())) {
			QAccessibleEvent event(item, QAccessible::Focus);
			QAccessible::updateAccessibility(&event);
		}
	});
	timer->start();
	return true;
}

void LineAccessibleList::resetCursor()
{
	m_cursor = QPersistentModelIndex();
}

int LineAccessibleList::cursorRow() const
{
	if (m_cursor.isValid() && m_cursor.model() == m_model.data())
		return m_cursor.row();
	QAbstractItemView *v = view();
	return v && v->currentIndex().isValid() ? v->currentIndex().row() : -1;
}

QAccessibleInterface *LineAccessibleList::childAt(int x, int y) const
{
	QAbstractItemView *v = view();
	if (!v)
		return nullptr;
	const QModelIndex index = v->indexAt(v->viewport()->mapFromGlobal(QPoint(x, y)));
	return index.isValid() ? itemAt(index.row()) : nullptr;
}

QAccessibleInterface *LineAccessibleList::itemAt(int row) const
{
	QAbstractItemView *v = view();
	if (!v || row < 0 || row >= rowCount())
		return nullptr;
	if (const QAccessible::Id id = m_items.value(row)) {
		if (QAccessibleInterface *item = QAccessible::accessibleInterface(id))
			return item;
	}
	auto *item = new LineAccessibleListItem(v, row);
	m_items.insert(row, QAccessible::registerAccessibleInterface(item));
	return item;
}

int LineAccessibleList::rowCount() const
{
	checkModel();
	QAbstractItemView *v = view();
	return v && v->model() ? v->model()->rowCount(v->rootIndex()) : 0;
}

bool LineAccessibleList::isRowSelected(int row) const
{
	QAbstractItemView *v = view();
	if (!v || !v->model())
		return false;
	QItemSelectionModel *sm = v->selectionModel();
	return sm && sm->isRowSelected(row, v->rootIndex());
}


namespace {

QAbstractItemView *packageList(QWidget *window, const char *modelClass, bool visibleOnly)
{
	for (QAbstractItemView *v : window->findChildren<QAbstractItemView *>()) {
		if (v->model() && inheritsClass(v->model(), modelClass) && (!visibleOnly || v->isVisible())
		    && v->model()->rowCount(v->rootIndex()) > 0)
			return v;
	}
	return nullptr;
}

QWidget *buttonWithIcon(QWidget *window, const char *icon)
{
	for (QWidget *w : window->findChildren<QWidget *>()) {
		if (w->isVisible() && inheritsClass(w, "LcButton") && iconSource(w).contains(QLatin1StringView(icon)))
			return w;
	}
	return nullptr;
}

// Tried this session to read emoji package names (there may be none: not again then).
bool g_emojiNamesTried = false;
// A refresh is under way.
bool g_namesRefreshing = false;

// Names known for every package of the strip? LINE loads the emoji strip only when its tab is
// opened, so emoji names are also missing while none were read yet.
bool packageNamesKnown(QWidget *window)
{
	if (packageNames(true).isEmpty() && !g_emojiNamesTried)
		return false;
	for (const auto &[model, emoji] : { std::pair { "StickerPackageModel", false }, std::pair { "SticonPackageModel", true } }) {
		QAbstractItemView *strip = packageList(window, model, false);
		if (!strip)
			continue;
		const QStringList &names = packageNames(emoji);
		if (names.size() != strip->model()->rowCount(strip->rootIndex()) - 1 || names.contains(QString()))
			return false;
	}
	return true;
}

// Reads every row of the "My stickers" list of stickers or emoji (both stay "visible" in their
// stack); returns how many names it got.
int readMyStickers(QWidget *window, bool emoji, bool quiet = false)
{
	QAbstractItemView *list = nullptr;
	for (QAbstractItemView *v : window->findChildren<QAbstractItemView *>()) {
		if (!v->isVisible() || !v->model() || !inheritsClass(v->model(), "ShopEditPackageModel")
		    || v->model()->rowCount(v->rootIndex()) == 0)
			continue;
		QObject *first = v->model()->index(0, 0, v->rootIndex()).data(kItemDataRole).value<QObject *>();
		if (inheritsClass(first, emoji ? "SticonEditPackageData" : "StickerEditPackageData"))
			list = v;
	}
	if (!list) {
		if (!quiet)
			a11yLog("[sticker] My stickers: no %s list", emoji ? "emoji" : "sticker");
		return 0;
	}
	const int rows = list->model()->rowCount(list->rootIndex());
	int read = 0;
	for (int row = 0; row < rows; ++row) {
		const QModelIndex idx = list->model()->index(row, 0, list->rootIndex());
		QObject *data = idx.data(kItemDataRole).value<QObject *>();
		list->scrollTo(idx);
		if (const QString name = firstPaintedLine(list, idx); !name.isEmpty()) {
			notePackageName(inheritsClass(data, "SticonEditPackageData"), row, name, rows);
			++read;
		}
	}
	list->scrollTo(list->model()->index(0, 0, list->rootIndex()));
	return read;
}

// The tabs of "My stickers" (stickers, emoji), left to right.
QList<QWidget *> myStickersTabs(QWidget *window)
{
	QList<QWidget *> tabs;
	QAbstractItemView *list = packageList(window, "ShopEditPackageModel", true);
	for (QWidget *panel = list ? list->parentWidget() : nullptr; panel && panel != window; panel = panel->parentWidget()) {
		for (QWidget *w : panel->findChildren<QWidget *>()) {
			if (w->isVisible() && inheritsClass(w, "LcCheckBox"))
				tabs << w;
		}
		if (tabs.size() >= 2)
			break;
		tabs.clear();
	}
	std::sort(tabs.begin(), tabs.end(), [window](QWidget *a, QWidget *b) {
		return a->mapTo(window, QPoint()).x() < b->mapTo(window, QPoint()).x();
	});
	return tabs;
}

} // namespace

void refreshStickerPackageNames(QWidget *stickerWindow)
{
	QPointer<QWidget> window(stickerWindow);
	if (g_namesRefreshing || !window || !window->isVisible() || packageNamesKnown(window))
		return;
	QWidget *editButton = buttonWithIcon(window, "edit_icon");
	if (!editButton)
		return;
	g_namesRefreshing = true;
	g_emojiNamesTried = true;
	const QPointer<QWidget> focus = QApplication::focusWidget();
	a11yLog("[sticker] reading the package names from My stickers");
	pressButtonQuietly(editButton);
	// Each step after LINE has shown and drawn the panel.
	QTimer::singleShot(400, window, [window, focus] {
		int read = readMyStickers(window, false);
		const QList<QWidget *> tabs = myStickersTabs(window);
		a11yLog("[sticker] My stickers: %d tabs", int(tabs.size()));
		if (tabs.size() >= 2)
			pressButtonQuietly(tabs.at(1));
		// LINE loads the emoji list a while after its tab is chosen: looked for every 200 ms.
		auto finish = [window, focus, tabs](int all) {
			if (tabs.size() >= 2 && tabs.first())
				pressButtonQuietly(tabs.first());
			if (QWidget *back = buttonWithIcon(window, "back_icon"))
				pressButtonQuietly(back);
			QTimer::singleShot(300, window, [window, focus, all] {
				g_namesRefreshing = false;
				if (focus && focus->isVisible())
					focus->setFocus(Qt::OtherFocusReason);
				a11yLog("[sticker] %d package names read; focus back on %s", all,
				        focus ? focus->metaObject()->className() : "nothing");
			});
		};
		auto poll = std::make_shared<std::function<void(int)>>();
		*poll = [window, read, finish, poll](int tries) {
			if (!window)
				return;
			const int emoji = readMyStickers(window, true, tries > 0);
			if (emoji > 0 || tries == 0) {
				finish(read + emoji);
				*poll = nullptr;
				return;
			}
			QTimer::singleShot(200, window, [poll, tries] {
				if (*poll)
					(*poll)(tries - 1);
			});
		};
		QTimer::singleShot(200, window, [poll] { (*poll)(10); });
	});
	// The flag is reset by the last step; a window closed meanwhile takes its timers along.
	QObject::connect(window, &QObject::destroyed, [] { g_namesRefreshing = false; });
}

} // namespace line_a11y
