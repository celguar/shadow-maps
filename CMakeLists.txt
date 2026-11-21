# ShadowMaps.dll -- the 1.12.1 client shadow-payload loader described in README.md.
# The only supported configuration is 32-bit: WoW.exe build 5875 is an i686 PE.
cmake_minimum_required(VERSION 3.10)

# Only C: src/mod.c is C written around the client's thiscall/__cdecl layouts,
# and shadowmaps.def exports the undecorated `Load` that VanillaFixes resolves
# with GetProcAddress(module, "Load"). A C++ build of the same file mangles
# that name, so the language comes from the file extension and the compiler
# flags below, and a configure that silently picked C++ would be a different
# program -- which is why nothing here lets CMake choose.
project(ShadowMaps LANGUAGES C)

# WoW.exe is 32-bit, so the injected DLL must be 32-bit: there is no valid
# 64-bit configure of this project. Fail at configure time rather than let one
# through, exactly as ClassicAPI's CMakeLists does -- an IDE that configures on
# its own (CMake Tools picks an architecture from a scanned kit) can otherwise
# produce a DLL that simply fails to load in the client, with no message here
# to point back at.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "ShadowMaps builds 32-bit only. This configure uses ${CMAKE_SIZEOF_VOID_P}-byte"
        " pointers. Delete the build directory, because CMake cannot switch platform in"
        " place, then configure with `cmake --preset win32` or `cmake -B build -A Win32`.")
endif()

# The static CRT, so the DLL imports only the operating system's own DLLs: the
# client's folder has no reason to carry vcruntime or ucrt DLLs beside it (and
# often cannot). Same reason ClassicAPI sets this.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# The .def is listed as a source, so CMake emits a quoted /DEF: on the link on
# every generator and relinks when the export list changes -- the same shape
# ClassicAPI uses for src/ClassicAPI.def. Keeping it a source rather than a
# linker flag means the exports and the DLL cannot drift apart silently.
add_library(ShadowMaps SHARED
            src/mod.c
            shadowmaps.def)

# /MT asks for the static CRT, /TC states the language (the extension already
# says it, and repeating it here keeps a generator that rewrites its own flags
# from quietly choosing C++).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /TC /MT")
set(CMAKE_C_FLAGS_RELEASE "/O2 /DNDEBUG /TC /MT")

# /NODEFAULTLIB:MSVCRT is the partner of the static-CRT request above, as in
# ClassicAPI's link: it keeps the dynamic-CRT import library out of the image.
# /MAP is here so a shipped DLL can be cited by offset the way its own log lines
# cite the client's addresses.
#
# /SUBSYSTEM is deliberately NOT pinned: /SUBSYSTEM:WINDOWS,5.0 is rejected for
# an IX86 image (the linker answers LNK4010 "invalid subsystem version 5.0"),
# because the 5.0 floor is what an x64 link uses to mean "Vista-era". A 32-bit
# DLL already carries the subsystem the client loads, so leaving it alone is the
# honest flag list here.
set(CMAKE_SHARED_LINKER_FLAGS "/NODEFAULTLIB:MSVCRT /MAP /INCREMENTAL:NO")
