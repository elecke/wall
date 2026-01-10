// adapted from cURL.
// https://github.com/curl/curl/blob/master/lib/curlx/strcopy.c
#ifndef STRCOPY_H
#define STRCOPY_H

#include <stddef.h>
#include <string.h>

#ifdef NDEBUG
#define DEBUG_ASSERT(condition) ((void)0)
#else
#include <assert.h>
#define DEBUG_ASSERT(condition) assert(condition)
#endif

static inline void strCopy(char *Dest, size_t DSize, const char *Src, size_t SLen)
{
    DEBUG_ASSERT(SLen < DSize);
    if (SLen < DSize)
    {
        memcpy(Dest, Src, SLen);
        Dest[SLen] = 0;
    }
    else if (DSize != 0)
    {
        Dest[0] = 0;
    }
}

#endif // STRCOPY_H
