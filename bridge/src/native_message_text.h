#pragma once

#include <QtCore/QMetaType>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>
#include <QtGui/QTextDocumentFragment>

namespace line_a11y {

// LINE 26.6 gives each message row a text of its own (Qt::AccessibleTextRole of ChatMessageModel):
// "Sender, text, time" for received messages, "text, time, Read" for the user's own, the whole reply
// ("X replied to Y's message ..."), full file names, and rows off screen too. Mentions in quotes are
// rich links. Strings only: other roles hold data objects. Empty when LINE gives nothing.
inline QString nativeMessageText(const QVariant &value)
{
	if (value.metaType().id() != QMetaType::QString)
		return {};
	QString text = value.toString();
	static const QRegularExpression markup(QStringLiteral(
	    "<(?:a\\s[^>]*href\\s*=|/?(?:p|br|span|div|b|i|strong|em|font)\\b)[^>]*>"),
	    QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression entity(QStringLiteral("&(?:amp|lt|gt|quot|apos|nbsp|#\\d+|#x[0-9a-fA-F]+);"));
	if (text.contains(markup) || text.contains(entity))
		text = QTextDocumentFragment::fromHtml(text).toPlainText();
	text.replace(QChar::ObjectReplacementCharacter, u' ');
	return text.simplified();
}

// What the bridge read from the drawn row, for what LINE's own text leaves out. Labels are already
// in LINE's language.
struct MessageRowParts {
	bool captured = false; // the row was on screen once
	bool own = false;
	QString sender;        // one-to-one chats with the sender name option: the other person
	QString kind;          // Sticker, Video, Voice message ... (also "Photo or sticker")
	QStringList lengths;   // a call's / voice message's length
	QString reactions;
};

// LINE's text with the bridge's parts: "You" before the user's own messages (LINE says nothing),
// the kind where LINE's text has no body (a sticker is ", 15.29") or only a length (a call is
// "00:07, 20.49"), the reactions at the end.
inline QString withMessageParts(QString text, const MessageRowParts &parts, const QString &you)
{
	if (!parts.captured)
		return text;
	if (!parts.kind.isEmpty()) {
		QStringList fields = text.split(QStringLiteral(", "));
		if (const qsizetype empty = fields.indexOf(QString()); empty >= 0) {
			fields[empty] = parts.kind;
			for (qsizetype i = 0; i < parts.lengths.size(); ++i)
				fields.insert(empty + 1 + i, parts.lengths.at(i));
		} else if (!parts.lengths.isEmpty() && !fields.contains(parts.kind)) {
			if (const qsizetype at = fields.indexOf(parts.lengths.first()); at >= 0)
				fields.insert(at, parts.kind);
		}
		text = fields.join(QStringLiteral(", "));
	}
	if (!parts.reactions.isEmpty() && !text.contains(parts.reactions))
		text += QStringLiteral(", ") + parts.reactions;
	const QString first = text.section(QStringLiteral(", "), 0, 0);
	if (parts.own && first != you)
		text.prepend(you + QStringLiteral(", "));
	else if (!parts.own && !parts.sender.isEmpty() && !text.startsWith(parts.sender))
		text.prepend(parts.sender + QStringLiteral(", "));
	return text;
}

} // namespace line_a11y
