// Keeps LINE's call process supplied with the bridge.
//
// LINE downloads its call process (Data\plugin\LineCall\<version>\LineCall.exe) some time after it
// was installed and puts updates into new version folders while it runs, so the add-on, which
// installs the bridge when LINE starts, can miss it: the incoming-call window then has no bridge.
// The bridge in LINE.exe copies itself next to every LineCall.exe that lacks it or has another
// build: at start-up (LINE starts LineCall about 10 s later) and every 30 s.

#pragma once

class QObject;

namespace line_a11y {

// Starts the copying (LINE.exe only); `parent` owns the timer.
void startCallBridgeSync(QObject *parent);

} // namespace line_a11y
