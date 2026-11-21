/* stdlib.h -- a STUB used only to syntax-check mod.c; see build.py --check.
 *
 * Same reasoning as tools-check/windows.h: this machine has no Windows SDK. The
 * real <stdlib.h> is where MSVC also defines NULL, so that define lives here
 * too; nothing else in this file is used by a shipped DLL.
 */
#ifndef SHADOWMOD_CHECK_STDLIB_H
#define SHADOWMOD_CHECK_STDLIB_H 1

#define NULL 0

#ifdef __cplusplus
extern "C" {
#endif

void *malloc(unsigned long long);
void free(void *);
int abs(int);

#ifdef __cplusplus
}
#endif

#endif
