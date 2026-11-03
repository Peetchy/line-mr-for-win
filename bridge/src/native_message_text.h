#pragma once

#include <QtCore/QMetaType>
#include <QtCore/QRegularExpression>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtGui/QTextDocumentFragment>

namespace line_a11y {

// LINE's AccessibleTextRole includes rich links in mentions/quoted messages.
// Accept actual strings only: other model roles can return opaque data objects.
// Read the entire value, never the diagnostics' 160-character preview.
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

} // namespace line_a11y
