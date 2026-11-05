#pragma once

#include <functional>

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
	text = text.simplified();
	// A forwarded video's file name is a content hash ("3B1141DE...5C.mp4"): noise when read.
	static const QRegularExpression hashName(QStringLiteral("^[0-9A-Fa-f]{24,}\\.[A-Za-z0-9]{2,5}$"));
	QStringList fields = text.split(QStringLiteral(", "));
	fields.removeIf([](const QString &field) { return hashName.match(field).hasMatch(); });
	return fields.join(QStringLiteral(", "));
}

// Time ("2:49 PM", "14:49", "14.49 น.", "下午 2:49", "午前 7:57") and read receipt ("Read", "Read 3",
// "อ่านแล้ว", "既読", "已讀") labels, as LINE draws them beside a message and ends its own text with.
inline bool isMessageTimeOrReceipt(const QString &text)
{
	static const QRegularExpression pattern(QStringLiteral(
		"^((([上下]午|午[前後])\\s?)?\\d{1,2}[:.]\\d{2}(\\s?([AaPp]\\.?[Mm]\\.?|น\\.?))?"
		"|Read( \\d+)?|อ่านแล้ว( \\d+)?|已[讀读](\\s?\\d+)?|既読(\\s?\\d+)?)$"));
	return pattern.match(text.trimmed()).hasMatch();
}

// What the bridge read from the drawn row, for what LINE's own text leaves out. Labels are already
// in LINE's language.
struct MessageRowParts {
	bool captured = false; // the row was on screen once
	bool own = false;
	QString sender;        // one-to-one chats with the sender name option: the other person
	QString kind;          // Sticker, Video, Voice message ... (also "Photo or sticker")
	QStringList lengths;   // a call's / voice message's length
	QStringList details;   // drawn lines LINE's text may lack: a file's size and deadline, a link's title
	QString reactions;
};

// Bridge labels withMessageParts() uses, in LINE's language.
struct MessageLabels {
	QString you;
	QString replyTo;            // "Reply to %1: %2"
	QString original;           // "Original message: %1"
	QStringList kindsToName;    // kinds LINE's text never names (Location, Contact)
	QString album;              // replaces LINE's photo word for an album
	QStringList photoWords;     // LINE's word for a photo ("Photo", "รูป")
	std::function<QString(const QString &)> duration; // a length as words ("7 seconds")
};

// LINE's reply sentence, shortened so the new text comes first:
// "X ตอบกลับข้อความของ Y ข้อความเดิมของ Y คือ Q ตอบกลับว่า R, 18.20 น."
// -> "X, ตอบกลับ Y: R, ข้อความเดิม: Q, 18.20 น.". Other texts unchanged.
inline QString shortReply(const QString &text, const MessageLabels &labels)
{
	if (labels.replyTo.isEmpty() || labels.original.isEmpty())
		return text;
	QStringList fields = text.split(QStringLiteral(", "));
	QStringList trailing;
	while (fields.size() > 1 && isMessageTimeOrReceipt(fields.last()))
		trailing.prepend(fields.takeLast());
	const QString body = fields.join(QStringLiteral(", "));
	// LINE 26.6 in its five languages (seen 2026-11-05); the sender may be absent (the user's own).
	static const QRegularExpression patterns[] = {
		QRegularExpression(QStringLiteral("^A reply (?:from (?<sender>.+?) )?to (?<quoted>.+?)'s message\\. \\k<quoted>'s original "
		                                  "message: (?<quote>.*?)\\. Reply: (?<reply>.*?)\\.?$"),
		                   QRegularExpression::DotMatchesEverythingOption),
		QRegularExpression(QStringLiteral("^(?:(?<sender>.+?) )?ตอบกลับข้อความของ (?<quoted>.+?) ข้อความเดิมของ \\k<quoted> คือ "
		                                  "(?<quote>.*?) ?ตอบกลับว่า (?<reply>.*)$"),
		                   QRegularExpression::DotMatchesEverythingOption),
		QRegularExpression(QStringLiteral("^(?<quoted>.+?)のメッセージへの(?:(?<sender>.+?)の)?リプライ。\\k<quoted>の元のメッセージ："
		                                  "(?<quote>.*?)。リプライ：(?<reply>.*?)。?$"),
		                   QRegularExpression::DotMatchesEverythingOption),
		QRegularExpression(QStringLiteral("^(?<sender>.*?)回复(?<quoted>.+?)的信息。\\k<quoted>的原信息是：(?<quote>.*?)。回复是："
		                                  "(?<reply>.*?)。?$"),
		                   QRegularExpression::DotMatchesEverythingOption),
		QRegularExpression(QStringLiteral("^(?<sender>.*?)回覆(?<quoted>.+?)的訊息。\\k<quoted>的原始訊息：(?<quote>.*?)。回覆內容："
		                                  "(?<reply>.*?)。?$"),
		                   QRegularExpression::DotMatchesEverythingOption),
	};
	for (const QRegularExpression &pattern : patterns) {
		const QRegularExpressionMatch m = pattern.match(body);
		if (!m.hasMatch())
			continue;
		QStringList out;
		if (const QString sender = m.captured(u"sender").trimmed(); !sender.isEmpty())
			out << sender;
		out << QString(labels.replyTo).arg(m.captured(u"quoted").trimmed(), m.captured(u"reply").trimmed());
		if (const QString quote = m.captured(u"quote").trimmed(); !quote.isEmpty())
			out << QString(labels.original).arg(quote);
		return (out + trailing).join(QStringLiteral(", "));
	}
	return text;
}

// LINE's text with the bridge's parts: "You" before the user's own messages (LINE says nothing),
// the kind where LINE's text has no body (a sticker is ", 15.29"), only a length (a call is
// "00:07, 20.49") or no kind (a location is its address), an album for LINE's "Photo", drawn details
// LINE's text lacks before the time, the reactions at the end.
inline QString withMessageParts(QString text, const MessageRowParts &parts, const MessageLabels &labels)
{
	text = shortReply(text, labels);
	if (parts.captured) {
		const auto spoken = [&labels](const QString &length) { return labels.duration ? labels.duration(length) : length; };
		QStringList fields = text.split(QStringLiteral(", "));
		if (!parts.kind.isEmpty()) {
			if (const qsizetype empty = fields.indexOf(QString()); empty >= 0) {
				fields[empty] = parts.kind;
				for (qsizetype i = 0; i < parts.lengths.size(); ++i)
					fields.insert(empty + 1 + i, spoken(parts.lengths.at(i)));
			} else if (!parts.lengths.isEmpty()) {
				if (const qsizetype at = fields.indexOf(parts.lengths.first()); at >= 0) {
					fields[at] = spoken(fields.at(at));
					if (!fields.contains(parts.kind))
						fields.insert(at, parts.kind);
				}
			} else if (parts.kind == labels.album && !labels.album.isEmpty()) {
				for (qsizetype i = 0; i < qMin<qsizetype>(2, fields.size()); ++i) {
					if (labels.photoWords.contains(fields.at(i))) {
						fields[i] = labels.album;
						break;
					}
				}
			} else if (labels.kindsToName.contains(parts.kind) && !fields.contains(parts.kind)) {
				// After the sender's name LINE puts before received messages.
				fields.insert(parts.own || fields.size() < 2 ? 0 : 1, parts.kind);
			}
		}
		const auto compact = [](QString s) { return s.remove(u' '); };
		qsizetype end = fields.size();
		while (end > 1 && isMessageTimeOrReceipt(fields.at(end - 1)))
			--end;
		for (const QString &detail : parts.details) {
			if (!detail.isEmpty() && !compact(fields.join(QString())).contains(compact(detail)))
				fields.insert(end++, detail);
		}
		text = fields.join(QStringLiteral(", "));
		if (!parts.reactions.isEmpty() && !text.contains(parts.reactions))
			text += QStringLiteral(", ") + parts.reactions;
	}
	const QString first = text.section(QStringLiteral(", "), 0, 0);
	if (parts.own && first != labels.you)
		text.prepend(labels.you + QStringLiteral(", "));
	else if (!parts.own && !parts.sender.isEmpty() && !text.startsWith(parts.sender))
		text.prepend(parts.sender + QStringLiteral(", "));
	return text;
}

// The sender a message text (withMessageParts()' result) starts with: its first field when more
// than the body and the time follow. "" for dates and notices.
inline QString messageSender(const QString &text)
{
	const QStringList fields = text.split(QStringLiteral(", "));
	qsizetype end = fields.size();
	while (end > 1 && isMessageTimeOrReceipt(fields.at(end - 1)))
		--end;
	return end >= 2 && end < fields.size() ? fields.first() : QString();
}

// With "sender's name only when it changes": `text` without its first field (the sender) when the
// row above is from the same sender.
inline QString withoutRepeatedSender(const QString &text, const QString &previousText)
{
	const QString sender = messageSender(text);
	if (sender.isEmpty() || sender != messageSender(previousText))
		return text;
	return text.section(QStringLiteral(", "), 1);
}

} // namespace line_a11y
