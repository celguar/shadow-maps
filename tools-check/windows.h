/* windows.h -- a STUB used only to syntax-check mod.c; see build.py --check.
 *
 * This machine's Visual Studio carries no Windows SDK headers at all (no
 * windows.h, stdio.h or stdlib.h exists under any include tree), so mod.c
 * cannot be preprocessed here. These few declarations let the real MSVC
 * compiler read mod.c and report its C errors, which is the one thing still
 * worth doing on a machine that cannot build the target. The shipped DLL is
 * built with the REAL headers, on a machine with an i686-capable MSVC: the
 * README's toolchain section says what this machine lacks (the Windows SDK
 * component and the x86-32 inline-asm assembler, so cl here is x64-only).
 * build.py puts this folder on INCLUDE only in --check mode, never when it
 * links a DLL.
 */
#ifndef SHADOWMOD_CHECK_STUB_H
#define SHADOWMOD_CHECK_STUB_H 1

typedef unsigned long DWORD;
typedef long LONG;             /* the real headers spell Interlocked*'s type this way */
typedef int BOOL;
typedef void *LPVOID;
typedef void *HMODULE;
typedef void *HANDLE;
typedef DWORD *LPDWORD;
typedef unsigned long long SIZE_T;

#define TRUE 1
#define FALSE 0
#define WINAPI __stdcall
#define MAX_PATH 260u

/* ClassicAPI's src/DllMain.cpp and this file both spell the entry point
 * `BOOL APIENTRY DllMain(...)`, and the real SDK headers supply APIENTRY. This
 * machine has no SDK, so the stub stands in for that one macro. */
#ifndef APIENTRY
#define APIENTRY
#endif

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 2

#define MEMRESIDENT_ 0x00001000u
#define MEMCOMMIT_ 0x00002000u
#define MEMMAP_ 0x00004000u
#define MEMORY_ACCESS_READ_ 0x00000001u
#define MEMORY_ACCESS_WRITE__ 0x00000002u
#define MEMORY_ACCESS_EXECUTE___ 0x00000004u

#ifdef __cplusplus
extern "C" {
#endif

__declspec(dllimport) void *VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
__declspec(dllimport) int VirtualProtect(void *, SIZE_T, DWORD, LPDWORD);
__declspec(dllimport) int FlushInstructionCache(void *, const void *, SIZE_T);
__declspec(dllimport) void *GetCurrentProcess(void);
__declspec(dllimport) unsigned GetModuleFileNameA(void *, char *, unsigned);
__declspec(dllimport) void *GetModuleHandleW(const void *);
__declspec(dllimport) void DisableThreadLibraryCalls(void *);
__declspec(dllimport) void *CreateThread(unsigned, unsigned, DWORD (*)(void *), void *, unsigned, void *);
__declspec(dllimport) int CloseHandle(void *);
__declspec(dllimport) unsigned GetTickCount(void);
__declspec(dllimport) unsigned Sleep(unsigned);
__declspec(dllimport) long InterlockedCompareExchange(volatile long *, long, long);
__declspec(dllimport) long InterlockedExchange(volatile long *, long);

#ifdef __cplusplus
}
#endif

#endif
