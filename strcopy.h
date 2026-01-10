// adapted from cURL.
// https://github.com/curl/curl/blob/master/lib/curlx/strcopy.c
#ifndef STRCOPY_H
#define STRCOPY_H

#include <stddef.h>
#include <string.h>

#ifdef NDEBUG
#define DEBUGASSERT(x) ((void)0)
#else
#include <assert.h>
#define DEBUGASSERT(x) assert(x)
#endif

static inline void strcopy(char *dest, size_t dsize, const char *src, size_t slen)
{
    DEBUGASSERT(slen < dsize);
    if (slen < dsize)
    {
        memcpy(dest, src, slen);
        dest[slen] = 0;
    }
    else if (dsize != 0U)
    {
        dest[0] = 0;
    }
}

#endif // STRCOPY_H
