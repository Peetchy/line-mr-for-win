// LINE's own texts: the QML singleton XLT (module LineModule) holds every string LINE shows, in LINE's
// current language, under an enum key (XLTKey: "TalkChatlistFloatingCreateChat" = "Create chat").
// Labels of the bridge that name a LINE control use LINE's wording where a key is known (label()).

#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

namespace line_a11y {

// LINE's text for an XLTKey name; empty when it is unknown (another LINE version), not loaded yet,
// or outside LINE.exe.
QString lineText(const char *key);

// Every key with its text ("key<TAB>text", line breaks as \n), for diagnostics.
QStringList lineTextTable();

} // namespace line_a11y
