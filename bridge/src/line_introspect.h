// Introspection helpers for LINE's custom Lc* widgets (LINE for Windows, Qt 6.6.3, x64).
//
// Unlike the macOS bridge, nothing here depends on reverse-engineered memory offsets: text
// that LINE keeps outside the meta-object system (LcText, LcTextField hints) is recovered by
// recording what the widget paints (text_capture.h).

#pragma once

#include <QtCore/QList>
#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include <initializer_list>

QT_BEGIN_NAMESPACE
struct QMetaObject;
class QObject;
class QWidget;
QT_END_NAMESPACE

namespace line_a11y {

// --- Class helpers --------------------------------------------------------------------------

bool inheritsClass(const QMetaObject *mo, const char *className);
bool inheritsClass(const QObject *object, const char *className);
bool inheritsAnyClass(const QObject *object, std::initializer_list<const char *> classNames);
// Nearest ancestor (excluding `w`) whose class or superclass is `className`.
QWidget *ancestorInheriting(const QWidget *w, const char *className);

// --- Properties -----------------------------------------------------------------------------

QVariant readProperty(const QObject *o, const char *name);
QString stringProp(const QObject *o, const char *name); // cleaned; "" unless a QString property
int boolProp(const QObject *o, const char *name);       // 1 / 0, -1 if absent

// --- Text extraction ------------------------------------------------------------------------

// Collapses whitespace, converts rich text to plain text and drops object-replacement chars.
QString cleanText(const QString &text);

// What `w` itself paints as text (not its children), cached briefly. Empty for hidden widgets.
QString paintedText(QWidget *w);

// Which of LINE's known icons `w` itself paints (knownIcon() names, "chatroom-call"), from the
// same cached rendering as paintedText(); "" if none.
QString paintedIcon(QWidget *w);

// Text of an LcText ("text" property if a subclass declares one, else painted text).
QString extractLcTextString(QWidget *w);

// w->toolTip() (LcWidget keeps its custom tooltip there on Windows too, when set).
QString extractLcWidgetToolTip(QWidget *w);

// Icon resource of an LcButton / LcImage ("image://common/system-close.svg"), or "".
QString iconSource(const QWidget *w);

// Converts icon resource names ("image://login/login-help.svg", "eye-show", ...) to a
// human-readable name in LINE's UI language. Returns "" if nothing useful remains.
QString humanizeIconName(const QString &iconName);
// The arrows of a chat's search bar (both draw down-arrow.svg, the one pointing up rotated by 180
// degrees): -1 previous (older) result, 1 next (newer) result, 0 any other widget.
int chatSearchArrow(const QWidget *w);

// Converts identifiers like "accessUrlTextField" / "btn_close" to "Access url" / "Close".
QString humanizeIdentifier(const QString &identifier);

// Best-effort visible text of a single widget (QLabel, QAbstractButton, "text" property,
// LcText, accessibleName). Password fields never contribute.
QString widgetText(QWidget *w);

struct PositionedText {
	QPoint pos; // top-left in the coordinate system of the collection root
	QSize size;
	QString text;
};

// Visible descendant texts of `root` in reading order (visual lines top-to-bottom, each line
// left-to-right).
QList<PositionedText> collectPositionedChildTexts(QWidget *root);

// Same, joined into one string (consecutive duplicates removed).
QString collectVisibleChildTexts(QWidget *root, const QString &separator = QStringLiteral(", "));

// "33", "999+", "(27)": unread / member count badges rather than labels.
bool isBadgeText(const QString &text);

// Text of the closest visible sibling label on the same row (to the right first, then left),
// or directly above. Used for checkboxes / inputs whose caption is a separate LcText.
// The LcText or LcCheckBox right above `w` (within 40 px, starting at most 8 px right of w's left edge),
// searched from w's parent outwards: labels of combo boxes and fields in Settings.
QString labelAboveText(QWidget *w);
QString nearbyLabelText(QWidget *w);

// --- Links --------------------------------------------------------------------------------

// The first web address ("https://...", "www....") in `lines` (text lines as LINE draws them, top to
// bottom; an address that ends its line and goes on in the next ones without spaces was wrapped
// by LINE and is joined). "" if there is none; "www." addresses get "http://".
QString firstWebAddress(const QStringList &lines);

// Opens an http(s) address in the user's default browser through Windows (not through Qt, where
// LINE may have registered handlers of its own). False if it is not http(s) or Windows refused.
bool openInBrowser(const QString &url);

// --- Localisation ---------------------------------------------------------------------------

// LINE's own UI language (its setting, not Windows'): bridge-generated labels follow it.
enum class UiLanguage { English, Thai, Japanese, SimplifiedChinese, TraditionalChinese };
UiLanguage uiLanguage();

// Bridge-generated label in LINE's UI language.
QString label(const char *english);

} // namespace line_a11y
