#pragma once

#include <QString>

// Desktop (OS) notifications. On Linux with D-Bus available (the same dependency
// MPRIS uses) this posts a real freedesktop notification; otherwise it falls back
// to an in-app toast so callers never have to branch. Use for events the user
// should see even when the window isn't focused (import complete / failed).
namespace Notifier {
void notify(const QString &title, const QString &body);
}
