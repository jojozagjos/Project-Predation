#pragma once

namespace pred
{

// Winsock has to be started once per process and stopped when the last user is done with it, and
// nothing on POSIX does. Every part of the engine that opens a socket needs the same refcount, so
// it lives here rather than inside whichever one was written first.
//
// Getting this wrong is quiet rather than loud: socket() simply returns an invalid handle and
// everything above reports "no socket", which is what the relay did when it was the first thing in
// the process to want one.
class SocketSystem
{
public:
    // False when Winsock could not be started at all. Balanced with Release.
    static bool Acquire();
    static void Release();
};

} // namespace pred
