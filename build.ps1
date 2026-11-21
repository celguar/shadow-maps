# build.ps1 -- front end for build.py, the no-CMake fallback; run it from this
# folder:
#   cd I:\code\wow\misc\salmiak-shadow-mod
#   powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1
#
# The canonical build is CMake -- see README.md and CMakeLists.txt, which mirror
# ClassicAPI's project and state the requirement in ClassicAPI's own words. This
# fallback exists for a machine that has MSVC but no CMake, and because it names
# each missing toolchain piece by filename when a build cannot be done at all.
#
# Why two entry points: the real work belongs in build.py, because cl.exe needs
# its INCLUDE/LIB/PATH environment and this box's PowerShell will not set
# environment variables at all ($env.X = ... fails here with "PropertyNotFound"
# and $env['X'] = ... with "NullArray", and $env itself reads as NULL). Python
# can set them, so build.py is the build; this script is here so the flags below
# are visible to a reader who would rather look at PowerShell than at Python,
# and so a Windows user has one obvious command to type.
#
# What build.py actually runs, and why each flag is there:
#
#   /TC                    compile src/mod.c as C. The file is C (thiscall and
#                          __cdecl layouts, no C++ name mangling), and in C mode
#                          Load compiles to _Load -- the symbol shadowmaps.def
#                          exports as the plain "Load" VanillaFixes resolves with
#                          GetProcAddress(module, "Load").
#   /O2 /W3                optimize; warnings kept at a level that still reads.
#   /D_CRT_SECURE_NO_WARNINGS  quiet the CRT's deprecated-name nagging, the way
#                          ClassicAPI's project does.
#   /MT                    static CRT, so the DLL imports only the operating
#                          system's own DLLs. A /MD build would demand vcruntime
#                          DLLs beside the client, which the client's folder has
#                          no reason to carry (and often lacks). ClassicAPI's
#                          CMakeLists.txt sets /MT for the same reason.
#   /LD /Feout\ShadowMaps.dll /Foout\   the DLL, objects out of the source tree.
#   /link /DEF:shadowmaps.def
#         /MACHINE:IX86    32-bit: WoW.exe build 5875 is an i686 PE, so an x64
#                          DLL simply cannot be loaded by it.
#         /BASE:0x14000000 preferred base, cosmetic: the DLL relocates, and the
#                          address the detour resumes at is a runtime value, so
#                          relocation cannot break the hook.
#         /INCREMENTAL:NO  a fixed layout, no import thunks in the image.
#         /NODEFAULTLIB:MSVCRT  keep the dynamic-CRT import library out of the
#                          link, the partner of /MT.
#         /MAP             out\ShadowMaps.map, so a shipped DLL can be cited by
#                          offset the way its log lines cite the client.
#
# /SUBSYSTEM is not asked for: /SUBSYSTEM:WINDOWS,5.0 is rejected for an IX86
# image (the linker answers LNK4010 "invalid subsystem version 5.0"), because
# that 5.0 floor is an x64-link thing; a 32-bit DLL carries the subsystem the
# client already loads.
#
# Exit code is the build's: 0 means out\ShadowMaps.dll exists and every verify_dll.py
# check passed.

$source = 'src\mod.c'
if (-not (Test-Path $source)) {
    Write-Output 'run this from the mod folder: it wants src\mod.c next to it'
    exit 1
}

$script = 'build.py'
if (-not (Test-Path $script)) {
    Write-Output 'build.py is missing: build.ps1 is only its front end'
    exit 1
}

python build.py

$built = $LASTEXITCODE
if ($built -ne 0) {
    Write-Output "build FAILED with code $built -- see README.md, toolchain section"
    exit $built
}

$dll = 'out\ShadowMaps.dll'
if (-not (Test-Path $dll)) {
    Write-Output 'the build reported success but out\ShadowMaps.dll is not there'
    exit 1
}

python verify_dll.py $dll

$checked = $LASTEXITCODE
if ($checked -ne 0) {
    Write-Output "the DLL built but verify_dll.py is not clean -- do not ship it"
    exit $checked
}

Write-Output 'built out\ShadowMaps.dll and every check passed'
exit 0
