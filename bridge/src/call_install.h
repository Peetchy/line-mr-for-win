// Keeps LINE's helper processes supplied with the bridge: the call process
// (Data\plugin\LineCall\<version>\LineCall.exe) and the photo / video viewer
// (Data\plugin\LineMediaPlayer\<version>\LineMediaPlayer.exe).
//
// LINE downloads them some time after it was installed and puts updates into new version folders
// while it runs, so the add-on, which installs the bridge when LINE starts, can miss them: their
// windows then have no bridge. The bridge in LINE.exe copies itself next to every helper that lacks
// it or has another build: at start-up (LINE starts its helpers about 10 s later) and every 30 s.

#pragma once

class QObject;

namespace line_a11y {

// Starts the copying (LINE.exe only); `parent` owns the timer.
void startCallBridgeSync(QObject *parent);

} // namespace line_a11y
