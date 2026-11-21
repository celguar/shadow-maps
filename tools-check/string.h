/* string.h -- a STUB used only to syntax-check mod.c; see build.py --check.
 *
 * Same reasoning as tools-check/windows.h: this machine has no Windows SDK, so
 * the real header is unavailable and these declarations stand in for it while
 * the compiler reads mod.c. Never used when a DLL is actually linked.
 *
 * memcpy/memset/strlen are compiler intrinsics in MSVC, so their stub shapes
 * must match what the target's own <string.h> would give or the compiler
 * complains about a conflicting intrinsic (C4391). The check runs on the host's
 * x64 target, where size_t is unsigned long long.
 */
#ifndef SHADOWMOD_CHECK_STRING_H
#define SHADOWMOD_CHECK_STRING_H 1

#ifdef __cplusplus
extern "C" {
#endif

void *memcpy(void *, const void *, unsigned long long);
void *memset(void *, int, unsigned long long);
int memcmp(const void *, const void *, unsigned long long);
unsigned long long strlen(const char *);

#ifdef __cplusplus
}
#endif

#endif
