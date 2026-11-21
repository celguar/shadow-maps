#!/usr/bin/env python3
r"""
build.py -- compile ShadowMaps.dll, the 32-bit DLL mod that draws Salmiak's
baked shadow maps inside the 1.12.1 client (build.ps1 is a PowerShell front
end for this same script; either entry point works).

Why a script at all: CMake (CMakeLists.txt, CMakePresets.json) is the canonical
build and README.md's commands use it. build.py is the no-CMake fallback: it
finds the compiler by wildcard, calls it directly, and sets the INCLUDE/LIB/PATH
environment cl.exe normally gets from the IDE -- and when a piece the build
needs is not installed, it names that file rather than leaving a C1083 or an
LNK1104 to be guessed at later.

Flags (their reasoning is written out in build.ps1, which this script mirrors;
keep the two in sync if you change one):

    /TC                     C, not C++: mod.c is C (thiscall/__cdecl layouts,
                            no mangling), and in C mode Load becomes _Load, the
                            symbol shadowmaps.def exports as the plain "Load"
                            VanillaFixes resolves with GetProcAddress.
    /O2 /W3                 optimize; warnings kept readable.
    /D_CRT_SECURE_NO_WARNINGS
                            the CRT deprecation nagging, as ClassicAPI sets it.
    /MT                     static CRT, so the DLL imports only OS DLLs. A /MD
                            build would demand vcruntime140.dll next to the
                            client, which the client's folder has no reason to
                            carry.
    /LD /Fe<out>            the DLL.
    /link /DEF:shadowmaps.def
          /MACHINE:IX86     the client is an i686 PE.
          /BASE:0x14000000  cosmetic: the DLL relocates and the resume address
                            is a runtime global, so relocation is safe.
          /INCREMENTAL:NO   fixed layout.
          /NODEFAULTLIB:MSVCRT  keep the dynamic-CRT import lib out of the link.
          /MAP              a map file, so a shipped DLL can be cited by offset.
          (no /SUBSYSTEM:WINDOWS,5.0: the linker rejects it for an IX86 image,
           LNK4010 "invalid subsystem version 5.0" -- that floor is an x64-link
           concept, and a 32-bit DLL carries the subsystem the client loads.)

Usage
    python build.py                       build, default: this folder
    python build.py <dir to build in>
    python build.py --check               syntax-check mod.c only (see below)
    powershell -NoProfile -File build.ps1 (run from this folder)

--check is a compile-only pass: no /LD, nothing linked. It uses the real headers
when this machine has a Windows SDK and falls back to this folder's tools-check/
stubs when it has only a compiler, so on a machine without the SDK it is still
worth running: it reports mod.c's real C errors. It is NOT a green light for
shipping -- it says nothing about the i686 link, and README.md's build section
is where the canonical CMake command line is.
"""

import glob
import os
import subprocess
import sys

# The compiler that can produce an i686 image: cl's own folder says which
# architecture it targets (bin\<host>\<target>), so the build wants a
# Host*\x86 driver, not the Hostx64\x64 one the IDE uses for x64 builds. On a
# VS install without the 32-bit target the fallback pattern still finds a
# compiler, and the diagnostic below then names what is missing.
PATTERNS = [
    r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x86\cl.exe",
    r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x86\cl.exe",
    r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx86\x86\cl.exe",
    r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Host*\x86\cl.exe",
    r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe",
]

FLAGS = [
    "/TC",
    "/nologo",
    "/O2",
    "/W3",
    "/D_CRT_SECURE_NO_WARNINGS",
    "/MT",
    "/I.",
    "/LD",
    r"/Feout\ShadowMaps.dll",
    "/Fo" + "out\\",
    "/link",
    "/DEF:shadowmaps.def",
    "/MACHINE:IX86",
    "/BASE:0x14000000",
    "/INCREMENTAL:NO",
    # /SUBSYSTEM:WINDOWS,5.0 is NOT asked for: the linker rejects it for an
    # IX86 image (LNK4010 "invalid subsystem version 5.0"), because the 5.0
    # floor is what an x64 link uses to mean "Vista-era". A 32-bit DLL carries
    # the subsystem the client already loads, so asking here would only add a
    # warning line to every build.
    "/NODEFAULTLIB:MSVCRT",
    r"/MAP:out\ShadowMaps.map",
]

# Windows SDK import libraries for the i686 target: the CRT ones live beside
# the compiler, the operating-system ones (kernel32, ntdll, ...) live in the
# Windows SDK's own tree. Both are needed to link, and naming them is what
# makes a machine without the SDK fail with a sentence instead of LNK1104.
SDK_PATTERNS = [
    r"C:\Program Files (x86)\Windows Kits\*\Lib\*\um\x86",
    r"C:\Program Files (x86)\Windows Kits\*\Lib\*\ucrt\x86",
]
SDK_INCLUDE_PATTERNS = [
    r"C:\Program Files (x86)\Windows Kits\*\Include\*\um",
    r"C:\Program Files (x86)\Windows Kits\*\Include\*\ucrt",
    r"C:\Program Files (x86)\Windows Kits\*\Include\*\shared",
]
NEED_LIBS = ("kernel32.lib", "ntdll.lib")


def find_compiler():
    for pattern in PATTERNS:
        hits = sorted(glob.glob(pattern))
        if hits:
            return hits[0]
    return None


# compile-only flags for --check: no /LD and nothing to link, so the host's own
# cl can read mod.c and report its C errors. (/MACHINE:I386 is not even tried
# here: it is a linker option, and as a driver option cl answers D9002 "unknown
# parameter" and quietly keeps producing x64 objects.) The header search comes
# from find_header_dirs: a real SDK wins, this folder's tools-check/ stubs are
# the fallback for a machine that has no SDK at all.
CHECK_FLAGS = [
    "/TC",
    "/c",
    "/nologo",
    "/O2",
    "/W3",
    "/D_CRT_SECURE_NO_WARNINGS",
    "/I.",
]


def find_header_dirs(folder):
    r"""Every include directory the build should offer cl, best first.

    The Windows SDK and the compiler's CRT tree are separate installs, and this
    DLL needs both: <windows.h> comes from the SDK's own tree (Windows Kits\\*
    \Include\*\um, plus ucrt for the C library headers and shared for
    winapifamily.h), while the compiler's include tree carries its side of the CRT. A
    machine with a compiler but no SDK has no real headers at all, and this
    folder's tools-check/ is the labelled stub that lets --check read mod.c
    anyway. A stub is never good enough to link a DLL against.
    """
    hits = []
    for pattern in SDK_INCLUDE_PATTERNS:
        hits += sorted(glob.glob(pattern))
    for pattern in (r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\include",
                    r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\include"):
        hits += sorted(glob.glob(pattern))
    stubs = os.path.join(folder, "tools-check")
    if (not any(os.path.isfile(os.path.join(d, "windows.h")) for d in hits)
            and os.path.isfile(os.path.join(stubs, "windows.h"))):
        hits.append(stubs)
    return hits


def find_lib_dirs(cl):
    """Library search dirs for the i686 link: the SDK's, then the compiler's.

    kernel32.lib and ntdll.lib are Windows SDK import libraries, not compiler
    ones: they live in Windows Kits\\*\\Lib\\*\\um\\x86, and the CRT ones
    (libcmt.lib for /MT) live beside the compiler. A link that cannot see the
    first set answers LNK1104 against every Win32 call this mod makes, which
    reads as a code problem rather than a missing install -- so main() checks
    for them by name before running the link at all.
    """
    hits = []
    for pattern in SDK_PATTERNS:
        hits += sorted(glob.glob(pattern))
    # cl lives in ...\MSVC\<version>\bin\<host>\<target>: three levels up from
    # the target folder is the MSVC\<version> folder that owns lib/
    msvc = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(cl))))
    for d in (os.path.join(msvc, "lib", "x86"), os.path.join(msvc, "lib")):
        if os.path.isdir(d):
            hits.append(d)
    return hits


def decode_cl(text):
    """cl here speaks a code page, not UTF-8; try the ones this box answers with."""
    for cp in ("cp866", "cp1251", "gbk", "cp437"):
        try:
            return text.decode(cp)
        except LookupError:
            continue
    return text.decode("ascii", "replace")


def check(folder, cl):
    """Syntax-check src/mod.c: compile only, host architecture, no link."""
    dirs = find_header_dirs(folder)
    if not dirs:
        print("nothing on this machine resolves <windows.h>: the check cannot run")
        return 1
    env = dict(os.environ)
    env["INCLUDE"] = ";".join(dirs)
    print("check INCLUDE=%s" % env["INCLUDE"])
    done = subprocess.run([cl] + CHECK_FLAGS + ["/Fo" + "out" + os.sep + "check.obj",
                                                "src" + os.sep + "mod.c"],
                          env=env, cwd=folder, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    print(decode_cl(done.stdout))
    return done.returncode


def main(argv):
    folder = None
    for a in list(argv):
        if a == "--check":
            argv = [x for x in argv if x != "--check"]
            folder = argv[0] if argv else os.path.dirname(os.path.abspath(__file__))
            os.chdir(folder)
            if not os.path.isdir("out"):
                os.mkdir("out")
            cl = find_compiler()
            if not cl:
                print("no cl.exe under any Visual Studio VC\\Tools\\MSVC tree")
                return 1
            print("check with: %s" % cl)
            code = check(folder, cl)
            print("check exit %d" % code)
            print("check only compiles: it cannot say the i686 link works, that"
                  " is what the real build plus verify_dll.py are for")
            return code
    folder = argv[0] if argv else os.path.dirname(os.path.abspath(__file__))
    os.chdir(folder)
    for needed in ("src" + os.sep + "mod.c", "shadowmaps.def", "shadow_dat.h"):
        if not os.path.isfile(needed):
            print("missing %s: nothing to build" % needed)
            return 1

    cl = find_compiler()
    if not cl:
        print("no cl.exe under any Visual Studio VC\\Tools\\MSVC tree")
        print("set your compiler's path in PATTERNS, or build with the project you have")
        return 1
    print("compiler: %s" % cl)

    # cl.exe is useless without its include tree, its import libraries and its
    # own bin directory. Two checks below name the missing piece by filename
    # BEFORE running the link, because on a machine without the Windows SDK the
    # raw failure is C1083 "cannot open include file: windows.h" and then LNK1104
    # against every Win32 call, which reads as a code fault rather than a missing
    # install.
    bin_dir = os.path.dirname(cl)
    # cl lives in ...\MSVC\<version>\bin\<host>\<target>: three levels up from
    # the target folder is the MSVC\<version> folder that owns include/ and lib/
    msvc = os.path.dirname(os.path.dirname(os.path.dirname(bin_dir)))
    hdr = find_header_dirs(folder)
    libs = find_lib_dirs(cl)
    env = dict(os.environ)
    env["INCLUDE"] = ";".join(hdr)
    env["LIB"] = ";".join(libs)
    env["PATH"] = bin_dir + os.sep + env.get("PATH", "")
    # A 32-bit build also wants the sibling tool trees (the driver and the
    # linker look the target's own tools up by name, and a VS install keeps the
    # x86-32 set in its own Hostx64\x86 folder rather than beside the x64
    # compiler), so every sibling bin folder goes on PATH. mod.c has no inline
    # asm -- the detour is emitted bytes -- so nothing here depends on an
    # assembler, but a VS install that lacks the 32-bit target still cannot
    # produce the DLL.
    hops = ("Hostx64" + os.sep + "x86", "Hostx86" + os.sep + "x86",
            "Hostx64" + os.sep + "x64")
    for hop in hops:
        other = os.path.join(msvc, "bin", hop)
        if os.path.isdir(other):
            env["PATH"] = other + os.sep + env["PATH"]
            print("sibling tool tree: %s" % other)
    print("INCLUDE=%s" % env["INCLUDE"])
    print("LIB=%s" % env["LIB"])

    # The two things an i686 link needs and a machine can be missing. Named by
    # file, because those two names are what the requirement in README.md is
    # stated in terms of.
    missing = []
    if not any(os.path.isfile(os.path.join(d, "windows.h")) for d in hdr):
        missing.append("<windows.h> in no include dir: the Windows SDK headers"
                       " are not installed (they live in Windows Kits\\*\\Include"
                       "\\*\\um, not in the compiler's own include tree)")
    def has_lib(name):
        """Is that import library in any of the search dirs? Case-insensitive:
        the SDK's own files answer kernel32.Lib, not kernel32.lib, and link
        resolves them either way, so a case-sensitive check here would be a
        false alarm on a machine that can link perfectly well."""
        for d in libs:
            try:
                seen = os.listdir(d)
            except OSError:
                continue
            for f in seen:
                if f.lower() == name.lower():
                    return True
        return False

    for need in NEED_LIBS:
        if not has_lib(need):
            missing.append("%s for i686 in no library dir: the Windows SDK's"
                           " i686 import libraries are not installed" % need)
    # cl's own folder names the architecture it emits: bin\Hostx64\x86 makes
    # i686 objects, bin\Hostx64\x64 does not. Asked for a target it cannot do,
    # the driver answers D9002 "unknown parameter" and still produces an x64
    # image, which is why this is checked before the link rather than left to
    # verify_dll.py's machine-type check afterwards.
    if os.path.basename(bin_dir) != "x86":
        missing.append("the compiler found targets %s, not i686: this VS install"
                       " has no 32-bit target (VC\\Tools\\MSVC\\*\\bin\\Host*\\x86"
                       " is absent)" % os.path.basename(bin_dir))
    if missing:
        for line in missing:
            print(line)
        print("what still works here: python build.py --check, which reads mod.c"
              " for C errors with stub headers -- it cannot produce an i686 DLL,"
              " and README.md's toolchain section says so")
        return 1

    out_dir = "out"
    if not os.path.isdir(out_dir):
        os.mkdir(out_dir)
    exe_out = os.path.join(out_dir, "ShadowMaps.dll")

    # the source name must come BEFORE /link: cl treats everything after /link as
    # linker options, so a source placed at the end is not seen as a source at all
    # and the driver stops with D8003 "absence of source file name".
    cut = FLAGS.index("/link")
    cmd = [cl] + FLAGS[:cut] + ["src" + os.sep + "mod.c"] + FLAGS[cut:]
    print(" ".join(cmd))
    done = subprocess.run(cmd, env=env)
    if done.returncode != 0:
        print("build FAILED: cl exit code %d" % done.returncode)
        return done.returncode
    if not os.path.isfile(exe_out):
        print("cl reported success but %s is not there: check the flags" % exe_out)
        return 1

    print("built %s  (%d bytes)" % (exe_out, os.path.getsize(exe_out)))
    print("verify it before shipping:")
    print("    python verify_dll.py %s" % exe_out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
