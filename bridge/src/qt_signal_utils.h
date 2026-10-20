// Helpers for LINE signals whose argument types are private to LINE (LcButton *,
// LcListDelegateWidget *, ...), which neither string-based nor functor-based QObject::connect()
// can express from outside LINE.

#pragma once

#include <QtCore/QObject>

#include <functional>

namespace line_a11y {

// Method index of the signal `signature` (normalised) on `sender`, or -1.
int signalIndex(const QObject *sender, const char *signature);

// Directly connects signal `signalIndex` of `sender` to `fn`, which receives Qt's raw argument
// array: args[0] is the return slot, args[1..n] point at the signal's arguments.
QMetaObject::Connection connectRaw(QObject *sender, int signalIndex, QObject *context,
                                   std::function<void(void **)> fn);

// Emits signal `signalIndex` of `sender` with the raw argument array `args` (args[0] = nullptr).
bool emitRaw(QObject *sender, int signalIndex, void **args);

// Emits `signature` if `sender` has it. `args` as for emitRaw(); nullptr for a signal without
// arguments. Returns false if the signal does not exist.
bool emitSignal(QObject *sender, const char *signature, void **args = nullptr);

// Is anybody connected to `signature` on `sender`?
bool isSignalConnected(const QObject *sender, const char *signature);

} // namespace line_a11y
