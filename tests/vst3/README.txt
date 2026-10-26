Tests for Reno's VST3 host
=========================

The host is src/audiofx/vst3host.cpp. It is written against Reno's own declarations of the VST3
interfaces, src/audiofx/vst3_abi.h; Reno does not need Steinberg's VST3 SDK to build. The tests come in
two kinds: those that need nothing but Reno, and those that need the SDK (to prove the declarations
right, and to run the host against plug-ins made from Steinberg's own code).

Nothing here writes into the source tree: every script takes the folder to build in.


1. Without the SDK:  run.sh
---------------------------

    tests/vst3/run.sh <folder for what is built> [plain|asan|tsan|comstyle]

builds
    testplug.cpp   a VST3 plug-in file written against vst3_abi.h: three effects (a stereo gain with a
                   side-chain bus and a separate controller, a mono-only effect, an "instrument" that
                   is one object) and a pretend window. It counts what the host does and notes
                   everything the host does that VST3 does not allow.
    hosttest.cpp   drives the host through describeAudioPluginFile / createAudioPlugin and checks
                   about a hundred things: describing, making, parameters, audio, state, the window,
                   the order of calls as the plug-in saw it, that nothing is left over at the end, and
                   two threads at once.
and runs hosttest three times, with the plug-in's factory limited to IPluginFactory3, 2 and 1.
    plain      normal build
    asan       -fsanitize=address,undefined
    tsan       -fsanitize=thread
    comstyle   host and plug-in built with the Windows flavour of interface ids and result codes
               (RENO_VST3_COM_STYLE=1), so that part of the host's code runs on Linux too
Each run ends with "N checks, 0 failed".

What this shows: the host and vst3_abi.h agree with each other, and the host behaves.
What it cannot show: that vst3_abi.h is right. A wrong interface id or a wrong order of virtual
functions would be wrong on both sides and go unnoticed. That is what part 2 is for.


2. With the SDK:  run_sdk.sh
----------------------------

    tests/vst3/run_sdk.sh <SDK folder> <folder for what is built>

<SDK folder> has to have pluginterfaces/, public.sdk/ and base/ in it:
    git clone https://github.com/steinbergmedia/vst3_pluginterfaces  <SDK folder>/pluginterfaces
    git clone https://github.com/steinbergmedia/vst3_public_sdk      <SDK folder>/public.sdk
    git clone https://github.com/steinbergmedia/vst3_base            <SDK folder>/base
(the SDK is under the MIT licence since version 3.8; it is only used here, it is not copied into Reno).
Last run against "VST3 SDK 3.8.1": vst3_pluginterfaces 4f547e8e, vst3_public_sdk 586dc5e6,
vst3_base fcf9da0b.

It does two things.

a) abicheck.cpp is compiled with the official headers next to vst3_abi.h. The compiler compares the
   16 bytes of every interface id, every result code and constant the host uses, the size and
   alignment of every struct, the offset and type of every member, and the signature of every virtual
   function; run, it compares the position of every virtual function in its table and the number of
   virtual functions of every interface. It ends with "N checks, 0 mismatches". With clang++ around,
   the tables the compiler lays out are compared a second way (compare_vtables.py).

   For Windows the same file is compiled with clang for a Windows target and the Visual Studio /
   Windows SDK headers (ids, constants, layouts and signatures are checked by compiling; nothing is
   run), and the tables clang prints are compared:
       clang++ --target=x86_64-pc-windows-msvc <the usual MSVC flags and include paths> -std=c++17 \
           -I<SDK folder> -Isrc/audiofx -Xclang -fdump-vtable-layouts -c tests/vst3/abicheck.cpp \
           -o <somewhere>/abicheck.obj > <somewhere>/vtables.txt
       python3 tests/vst3/compare_vtables.py <somewhere>/vtables.txt
   (also done with --target=i686-pc-windows-msvc, where the calling convention and 8-byte struct
   packing matter). With Visual Studio itself, compiling abicheck.cpp with cl.exe does the compile-time
   part; cl /d1reportAllClassLayout prints the tables.

b) Three plug-in files are built from Steinberg's sources and the host is tried on each with probe:
       adelay     the SDK's ADelay sample. A click has to come out one second later; with the Delay
                  parameter set to 0.01, at sample 479.
       sdkcheck   sdkcheckplug.cpp: the SDK's AudioEffect and EditController classes with the checks
                  of Steinberg's "HostChecker" sample (public.sdk/samples/vst/hostchecker/source/
                  hostcheck.cpp ...) run on every block, and that sample's bookkeeping of the order of
                  calls. It has to end with "SDKCHECK done: 0 error(s), 0 warning(s)".
       mda        the SDK's mda-vst3 sample: 34 effects and instruments in one file. Each is made, has
                  its parameters listed, is given audio, and has its state saved and loaded.
   Here Steinberg's code reads the structs Reno's host fills in and calls the objects Reno's host
   implements (parameter changes, event list, streams, messages, host application), on Linux.
   The host is built with -fsanitize=address,undefined for this (without the "vptr" check: to that
   check every object of a plug-in has the wrong class, because the host's declarations of the
   interfaces are not the plug-in's own, they only have the same layout).


3. probe.cpp: trying a plug-in by hand
--------------------------------------

    probe <plug-in file> [--set <parameter index>=<value>]... [--rate hz] [--block frames]
          [--seconds s] [--only <start of an effect's name>]

says what is in a plug-in file, makes every effect in it, lists the parameters, sends a click through
and says what came out, and saves and loads the state. It goes through describeAudioPluginFile and
createAudioPlugin, so it works for VST2 files as well. It opens no window.
It builds on Windows too (it and hosttest.cpp link with the Visual Studio libraries):
    compile together: tests/vst3/probe.cpp src/core/*.cpp src/audiofx/audioplugin.cpp
                      src/audiofx/vst2host.cpp src/audiofx/vst3host.cpp src/audiofx/dxhost.cpp
This is the quickest way to see how the host gets on with a real plug-in on Windows.

To run hosttest on Windows, build testplug.cpp as a DLL named RenoTest.vst3, put it in
RenoTest.vst3\Contents\x86_64-win\ and give hosttest that file and any DLL that is no plug-in.


What has and has not been run
-----------------------------
Run on 64-bit Linux (GCC, and Clang where it says so): everything above.
Compiled for Windows, never run: the host, hosttest, probe and testplug (clang with the Visual Studio
and Windows SDK headers and libraries); abicheck for 64-bit, 32-bit and ARM64 Windows.
Never run anywhere: a plug-in's real window (the test plug-in's window is a pretend one, and the SDK
samples used here have none); any plug-in that was not built from Steinberg's SDK samples; anything on
Windows.
