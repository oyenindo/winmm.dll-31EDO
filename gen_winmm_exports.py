#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Membuat winmm_exports.h : daftar forwarder export winmm.dll -> winmm.dll ASLI.

    python gen_winmm_exports.py                  (baca System32\\winmm.dll di Windows)
    python gen_winmm_exports.py C:\\path\\winmm.dll
    python gen_winmm_exports.py --fallback       (pakai daftar bawaan di bawah)

Fungsi yang di-override oleh winmm_proxy.c TIDAK ikut diforward.
"""
import os
import struct
import sys

OVERRIDES = {
    "midiOutGetNumDevs", "midiOutGetDevCapsA", "midiOutGetDevCapsW", "midiOutOpen",
    "midiOutClose", "midiOutShortMsg", "midiOutLongMsg", "midiOutPrepareHeader",
    "midiOutUnprepareHeader", "midiOutReset", "midiOutGetID", "midiOutMessage",
    "midiOutGetVolume", "midiOutSetVolume",
}

FALLBACK = """
CloseDriver DefDriverProc DriverCallback DrvGetModuleHandle GetDriverModuleHandle OpenDriver
PlaySound PlaySoundA PlaySoundW SendDriverMessage WOW32DriverCallback WOW32ResolveMultiMediaHandle
WOWAppExit aux32Message auxGetDevCapsA auxGetDevCapsW auxGetNumDevs auxGetVolume auxOutMessage
auxSetVolume joy32Message joyConfigChanged joyGetDevCapsA joyGetDevCapsW joyGetNumDevs joyGetPos
joyGetPosEx joyGetThreshold joyReleaseCapture joySetCapture joySetThreshold mci32Message
mciDriverNotify mciDriverYield mciExecute mciFreeCommandResource mciGetCreatorTask mciGetDeviceIDA
mciGetDeviceIDFromElementIDA mciGetDeviceIDFromElementIDW mciGetDeviceIDW mciGetDriverData
mciGetErrorStringA mciGetErrorStringW mciGetYieldProc mciLoadCommandResource mciSendCommandA
mciSendCommandW mciSendStringA mciSendStringW mciSetDriverData mciSetYieldProc mid32Message
midiConnect midiDisconnect midiInAddBuffer midiInClose midiInGetDevCapsA midiInGetDevCapsW
midiInGetErrorTextA midiInGetErrorTextW midiInGetID midiInGetNumDevs midiInMessage midiInOpen
midiInPrepareHeader midiInReset midiInStart midiInStop midiInUnprepareHeader
midiOutCacheDrumPatches midiOutCachePatches midiOutGetErrorTextA midiOutGetErrorTextW
midiStreamClose midiStreamOpen midiStreamOut midiStreamPause midiStreamPosition midiStreamProperty
midiStreamRestart midiStreamStop mixerClose mixerGetControlDetailsA mixerGetControlDetailsW
mixerGetDevCapsA mixerGetDevCapsW mixerGetID mixerGetLineControlsA mixerGetLineControlsW
mixerGetLineInfoA mixerGetLineInfoW mixerGetNumDevs mixerMessage mixerOpen mixerSetControlDetails
mmDrvInstall mmGetCurrentTask mmTaskBlock mmTaskCreate mmTaskSignal mmTaskYield mmioAdvance
mmioAscend mmioClose mmioCreateChunk mmioDescend mmioFlush mmioGetInfo mmioInstallIOProcA
mmioInstallIOProcW mmioOpenA mmioOpenW mmioRead mmioRenameA mmioRenameW mmioSendMessage mmioSeek
mmioSetBuffer mmioSetInfo mmioStringToFOURCCA mmioStringToFOURCCW mmioWrite mmsystemGetVersion
mod32Message mxd32Message sndPlaySoundA sndPlaySoundW tid32Message timeBeginPeriod timeEndPeriod
timeGetDevCaps timeGetSystemTime timeGetTime timeKillEvent timeSetEvent waveInAddBuffer
waveInClose waveInGetDevCapsA waveInGetDevCapsW waveInGetErrorTextA waveInGetErrorTextW
waveInGetID waveInGetNumDevs waveInGetPosition waveInMessage waveInOpen waveInPrepareHeader
waveInReset waveInStart waveInStop waveInUnprepareHeader waveOutBreakLoop waveOutClose
waveOutGetDevCapsA waveOutGetDevCapsW waveOutGetErrorTextA waveOutGetErrorTextW waveOutGetID
waveOutGetNumDevs waveOutGetPitch waveOutGetPlaybackRate waveOutGetPosition waveOutGetVolume
waveOutMessage waveOutOpen waveOutPause waveOutPrepareHeader waveOutReset waveOutRestart
waveOutSetPitch waveOutSetPlaybackRate waveOutSetVolume waveOutUnprepareHeader waveOutWrite
wid32Message wod32Message
""".split()


def read_exports(path):
    """Baca nama export dari file PE (DLL)."""
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        raise ValueError("bukan file PE")
    nsec, = struct.unpack_from("<H", d, pe + 6)
    optsz, = struct.unpack_from("<H", d, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", d, opt)
    dd = opt + (112 if magic == 0x20B else 96)
    exp_rva, exp_size = struct.unpack_from("<II", d, dd)
    if not exp_rva:
        return []
    secs = []
    so = opt + optsz
    for i in range(nsec):
        vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, so + i * 40 + 8)
        secs.append((va, max(vsz, rsz), rptr))

    def off(rva):
        for va, sz, rptr in secs:
            if va <= rva < va + sz:
                return rva - va + rptr
        raise ValueError("RVA di luar section")

    e = off(exp_rva)
    nfunc, nnames, aof, aon, aono = struct.unpack_from("<IIIII", d, e + 20)
    names = []
    for i in range(nnames):
        nrva, = struct.unpack_from("<I", d, off(aon) + 4 * i)
        p = off(nrva)
        names.append(d[p:d.index(b"\0", p)].decode("ascii"))
    return names


def write_header(names, out):
    names = sorted({n for n in names if n not in OVERRIDES})
    with open(out, "w", newline="\n") as f:
        f.write("/* DIBUAT OLEH gen_winmm_exports.py - jangan diedit manual */\n")
        for n in names:
            f.write(f'#pragma comment(linker, "/export:{n}=C:\\\\Windows\\\\System32\\\\winmm.{n}")\n')
    return len(names)


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "winmm_exports.h")
    names, src = FALLBACK, "daftar bawaan"
    if "--fallback" not in sys.argv:
        arg = next((a for a in sys.argv[1:] if not a.startswith("--")), None)
        cand = arg or os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "winmm.dll")
        if os.path.isfile(cand):
            try:
                got = read_exports(cand)
                if got:
                    names, src = got, cand
            except Exception as ex:
                print("Gagal baca", cand, "->", ex, "(pakai daftar bawaan)")
    n = write_header(names, out)
    print(f"{n} forwarder ditulis ke {out} (sumber: {src})")
