#pragma once

#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Stop a child process (we spawn yt-dlp) from flashing a console window on
// Windows. No-op on Linux/macOS, where GUI apps don't get a console anyway.
inline void suppressConsoleWindow(QProcess *proc)
{
#ifdef Q_OS_WIN
    proc->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#else
    Q_UNUSED(proc);
#endif
}
