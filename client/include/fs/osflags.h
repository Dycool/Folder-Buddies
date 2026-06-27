#pragma once

#include <fcntl.h>

namespace fb {

enum PortableFlags {
    FB_O_RDONLY = 0,
    FB_O_WRONLY = 1,
    FB_O_RDWR = 2,
    FB_O_ACCMODE = 3,
    FB_O_CREAT = 0x0100,
    FB_O_EXCL = 0x0200,
    FB_O_TRUNC = 0x0400,
    FB_O_APPEND = 0x0800,
};

inline int to_portable_flags(int f) {
    int r = f & 3; // access mode bits are the same everywhere (0/1/2)
#ifdef O_CREAT
    if (f & O_CREAT) r |= FB_O_CREAT;
#endif
#ifdef O_EXCL
    if (f & O_EXCL) r |= FB_O_EXCL;
#endif
#ifdef O_TRUNC
    if (f & O_TRUNC) r |= FB_O_TRUNC;
#endif
#ifdef O_APPEND
    if (f & O_APPEND) r |= FB_O_APPEND;
#endif
    return r;
}

inline int from_portable_flags(int f) {
    int r = f & 3;
#ifdef O_CREAT
    if (f & FB_O_CREAT) r |= O_CREAT;
#endif
#ifdef O_EXCL
    if (f & FB_O_EXCL) r |= O_EXCL;
#endif
#ifdef O_TRUNC
    if (f & FB_O_TRUNC) r |= O_TRUNC;
#endif
#ifdef O_APPEND
    if (f & FB_O_APPEND) r |= O_APPEND;
#endif
#ifdef O_BINARY
    r |= O_BINARY; // Windows: never translate bytes
#endif
    return r;
}

} // namespace fb
