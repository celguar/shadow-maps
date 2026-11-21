/* stdio.h -- a STUB used only to syntax-check mod.c; see build.py --check.
 *
 * Same reasoning as tools-check/windows.h: these declarations stand in for the
 * Windows SDK's own headers on a machine that has the compiler but no SDK, so
 * build.py --check can still read mod.c. When a real SDK exists, build.py
 * prefers it and the stubs are not consulted at all. They are never linked
 * against: a DLL always comes from the real headers.
 *
 * The prototypes below copy UCRT's shapes, not the standard C ones, because
 * that is what a 32-bit Windows DLL has to compile against: fread and fwrite
 * carry an ELEMENT SIZE argument (fread(buf, elementSize, count, stream)) and
 * return bytes. Copying the C89 three-argument form here would let mod.c
 * pass --check and then fail against the real header with C2198 "too few
 * arguments", which is exactly the drift these stubs exist to prevent.
 *
 * No size_t typedef here on purpose: MSVC predefines size_t itself (redeclaring
 * it is error C2371 "reserved word, must be typedef'ed"), and mod.c uses both
 * size_t and the Win32 SIZE_T, which the real windows.h supplies.
 */
#ifndef SHADOWMOD_CHECK_STDIO_H
#define SHADOWMOD_CHECK_STDIO_H 1

struct SHADOWMOD_FILE {
    int stub;
};
typedef struct SHADOWMOD_FILE FILE;

#define SEEK_SET 0
#define SEEK_END 2
#define EOF (-1)

#ifdef __cplusplus
extern "C" {
#endif

FILE *fopen(const char *, const char *);
int fclose(FILE *);
unsigned long long fread(void *, unsigned long long, unsigned long long, FILE *);
unsigned long long fwrite(const void *, unsigned long long, unsigned long long, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
int fprintf(FILE *, const char *, ...);
int snprintf(char *, unsigned long, const char *, ...);

#ifdef __cplusplus
}
#endif

#endif
