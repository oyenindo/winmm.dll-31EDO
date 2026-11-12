/*
 * winmm_proxy.c  -  winmm.dll pengganti ("Windows Multimedia Wrapper")
 * ==================================================================
 * Ditaruh di folder .exe target (mis. Domino.exe) sebagai winmm.dll.
 *
 *  - Semua fungsi winmm diteruskan (forward) ke winmm.dll asli di System32
 *    (lihat winmm_exports.h, dibuat oleh gen_winmm_exports.py).
 *  - Fungsi midiOut* di-override: ditambahkan SATU perangkat MIDI Out virtual
 *    (ID terakhir). Semua pesan yang dikirim ke perangkat itu diteruskan
 *    ke aplikasi Python lewat TCP 127.0.0.1:<port>. Byte mentah dikirim apa
 *    adanya, jadi nomor key > 127 (256 key) tidak terpotong driver.
 *  - Perangkat MIDI lain berjalan normal.
 *
 * Konfigurasi: winmm_proxy.ini di folder yang sama dengan DLL
 *      [proxy]
 *      port=5731
 *      name=Microtonal Virtual Port
 *
 * Protokol TCP (little-endian):
 *      'S' + u32 dwMsg              pesan pendek (status,d1,d2)
 *      'L' + u32 len + data         pesan panjang (SysEx)
 *      'R'                          reset
 *      'O' / 'C'                    port dibuka / ditutup
 *
 * Build (MSVC, "x64/x86 Native Tools Command Prompt"):
 *      build_winmm_proxy.bat
 */
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#include "winmm_exports.h"             /* forwarder untuk semua fungsi lain */

/* ---- export fungsi yang di-override ----
 * Fungsi dibuat dengan nama internal pxy_<nama> lalu diexport sebagai <nama>
 * (tanpa dekorasi, x86 & x64). Dengan begitu tidak bentrok dengan deklarasi
 * di mmsystem.h (tipe return berbeda antar versi SDK). */
#ifdef _WIN64
#define EXPORT_OVR(name, bytes) __pragma(comment(linker, "/export:" #name "=pxy_" #name))
#else
#define EXPORT_OVR(name, bytes) __pragma(comment(linker, "/export:" #name "=_pxy_" #name "@" #bytes))
#endif
EXPORT_OVR(midiOutGetNumDevs, 0)
EXPORT_OVR(midiOutGetDevCapsA, 12)
EXPORT_OVR(midiOutGetDevCapsW, 12)
EXPORT_OVR(midiOutOpen, 20)
EXPORT_OVR(midiOutClose, 4)
EXPORT_OVR(midiOutShortMsg, 8)
EXPORT_OVR(midiOutLongMsg, 12)
EXPORT_OVR(midiOutPrepareHeader, 12)
EXPORT_OVR(midiOutUnprepareHeader, 12)
EXPORT_OVR(midiOutReset, 4)
EXPORT_OVR(midiOutGetID, 8)
EXPORT_OVR(midiOutMessage, 16)
EXPORT_OVR(midiOutGetVolume, 8)
EXPORT_OVR(midiOutSetVolume, 8)

/* penanda supaya aplikasi Python bisa mengenali DLL ini */
__declspec(dllexport) const char Microtonal_WinMM_Proxy_Marker[] = "MICROTONAL_WINMM_PROXY_V1";

/* ---- tipe fungsi asli ---- */
typedef UINT     (WINAPI *PFN_GetNumDevs)(void);
typedef MMRESULT (WINAPI *PFN_GetCapsA)(UINT_PTR, LPMIDIOUTCAPSA, UINT);
typedef MMRESULT (WINAPI *PFN_GetCapsW)(UINT_PTR, LPMIDIOUTCAPSW, UINT);
typedef MMRESULT (WINAPI *PFN_Open)(LPHMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *PFN_Close)(HMIDIOUT);
typedef MMRESULT (WINAPI *PFN_Short)(HMIDIOUT, DWORD);
typedef MMRESULT (WINAPI *PFN_Hdr)(HMIDIOUT, LPMIDIHDR, UINT);
typedef MMRESULT (WINAPI *PFN_Reset)(HMIDIOUT);
typedef MMRESULT (WINAPI *PFN_GetID)(HMIDIOUT, LPUINT);
typedef MMRESULT (WINAPI *PFN_Msg)(HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR);
typedef MMRESULT (WINAPI *PFN_GetVol)(HMIDIOUT, LPDWORD);
typedef MMRESULT (WINAPI *PFN_SetVol)(HMIDIOUT, DWORD);

static struct {
    PFN_GetNumDevs numdevs;
    PFN_GetCapsA   capsA;
    PFN_GetCapsW   capsW;
    PFN_Open       open;
    PFN_Close      close;
    PFN_Short      shortmsg;
    PFN_Hdr        longmsg, prep, unprep;
    PFN_Reset      reset;
    PFN_GetID      getid;
    PFN_Msg        msg;
    PFN_GetVol     getvol;
    PFN_SetVol     setvol;
} R;

/* ---- state ---- */
static HINSTANCE        g_self;
static CRITICAL_SECTION g_cs;
static volatile LONG    g_ready = 0;
static int              g_port = 5731;
static WCHAR            g_nameW[MAXPNAMELEN] = L"Microtonal Virtual Port";
static CHAR             g_nameA[MAXPNAMELEN] = "Microtonal Virtual Port";

static char             g_vdummy;                  /* alamatnya = handle virtual */
#define VHANDLE ((HMIDIOUT)&g_vdummy)

static SOCKET           g_sock = INVALID_SOCKET;
static DWORD            g_lastTry = 0;
static int              g_wsa = 0;
static int              g_opened = 0;
static DWORD_PTR        g_cb = 0, g_cbInst = 0;
static DWORD            g_cbFlags = 0;

/* ---- init (lazy, di luar DllMain) ---- */
static void ensure_init(void)
{
    WCHAR p[MAX_PATH + 32];
    UINT n;
    HMODULE m;
    if (g_ready) return;
    EnterCriticalSection(&g_cs);
    if (!g_ready) {
        n = GetSystemDirectoryW(p, MAX_PATH);
        lstrcpyW(p + n, L"\\winmm.dll");
        m = LoadLibraryW(p);
        if (m) {
            R.numdevs  = (PFN_GetNumDevs)GetProcAddress(m, "midiOutGetNumDevs");
            R.capsA    = (PFN_GetCapsA)GetProcAddress(m, "midiOutGetDevCapsA");
            R.capsW    = (PFN_GetCapsW)GetProcAddress(m, "midiOutGetDevCapsW");
            R.open     = (PFN_Open)GetProcAddress(m, "midiOutOpen");
            R.close    = (PFN_Close)GetProcAddress(m, "midiOutClose");
            R.shortmsg = (PFN_Short)GetProcAddress(m, "midiOutShortMsg");
            R.longmsg  = (PFN_Hdr)GetProcAddress(m, "midiOutLongMsg");
            R.prep     = (PFN_Hdr)GetProcAddress(m, "midiOutPrepareHeader");
            R.unprep   = (PFN_Hdr)GetProcAddress(m, "midiOutUnprepareHeader");
            R.reset    = (PFN_Reset)GetProcAddress(m, "midiOutReset");
            R.getid    = (PFN_GetID)GetProcAddress(m, "midiOutGetID");
            R.msg      = (PFN_Msg)GetProcAddress(m, "midiOutMessage");
            R.getvol   = (PFN_GetVol)GetProcAddress(m, "midiOutGetVolume");
            R.setvol   = (PFN_SetVol)GetProcAddress(m, "midiOutSetVolume");
        }
        /* winmm_proxy.ini di sebelah DLL */
        {
            WCHAR ini[MAX_PATH + 32];
            WCHAR *slash;
            DWORD len = GetModuleFileNameW(g_self, ini, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                slash = wcsrchr(ini, L'\\');
                if (slash) {
                    lstrcpyW(slash + 1, L"winmm_proxy.ini");
                    g_port = (int)GetPrivateProfileIntW(L"proxy", L"port", 5731, ini);
                    GetPrivateProfileStringW(L"proxy", L"name", L"Microtonal Virtual Port",
                                             g_nameW, MAXPNAMELEN, ini);
                }
            }
            WideCharToMultiByte(CP_ACP, 0, g_nameW, -1, g_nameA, MAXPNAMELEN, NULL, NULL);
            g_nameA[MAXPNAMELEN - 1] = 0;
        }
        g_ready = 1;
    }
    LeaveCriticalSection(&g_cs);
}

/* ---- jaringan ---- */
static int try_connect_locked(void)
{
    SOCKET s;
    struct sockaddr_in a;
    u_long nb = 1, blk = 0;
    fd_set wf, ef;
    struct timeval tv;
    int one = 1;
    DWORD now = GetTickCount();

    if (g_sock != INVALID_SOCKET) return 1;
    if (g_lastTry && (DWORD)(now - g_lastTry) < 2000) return 0;   /* jangan spam */
    g_lastTry = now;
    if (!g_wsa) return 0;

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ioctlsocket(s, FIONBIO, &nb);
    if (connect(s, (struct sockaddr *)&a, sizeof a) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return 0;
    }
    FD_ZERO(&wf); FD_ZERO(&ef);
    FD_SET(s, &wf); FD_SET(s, &ef);
    tv.tv_sec = 0; tv.tv_usec = 150000;                              /* 150 ms */
    if (select(0, NULL, &wf, &ef, &tv) <= 0 || !FD_ISSET(s, &wf)) {
        closesocket(s);
        return 0;
    }
    ioctlsocket(s, FIONBIO, &blk);
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    g_sock = s;
    return 1;
}

static int send_all_locked(const unsigned char *b, int len)
{
    int off = 0, n;
    while (off < len) {
        n = send(g_sock, (const char *)b + off, len - off, 0);
        if (n == SOCKET_ERROR || n == 0) {
            closesocket(g_sock);
            g_sock = INVALID_SOCKET;
            return 0;
        }
        off += n;
    }
    return 1;
}

static void tx(const unsigned char *a, int alen, const unsigned char *b, int blen)
{
    EnterCriticalSection(&g_cs);
    if (try_connect_locked()) {
        if (send_all_locked(a, alen) && b && blen > 0)
            send_all_locked(b, blen);
    }
    LeaveCriticalSection(&g_cs);
}

static void tx1(unsigned char type)
{
    tx(&type, 1, NULL, 0);
}

/* ---- util ---- */
static int is_vh(UINT_PTR x)
{
    return x == (UINT_PTR)&g_vdummy;
}

static UINT vid(void)                       /* ID perangkat virtual = jumlah perangkat asli */
{
    ensure_init();
    return R.numdevs ? R.numdevs() : 0;
}

static int is_virtual_idorhandle(UINT_PTR x)
{
    return is_vh(x) || (x != (UINT_PTR)-1 && x == (UINT_PTR)vid() && x < 0x10000);
}

static void fire_cb(UINT msg, DWORD_PTR d1, DWORD_PTR d2)
{
    switch (g_cbFlags & CALLBACK_TYPEMASK) {
    case CALLBACK_FUNCTION:
        if (g_cb)
            ((void (CALLBACK *)(HMIDIOUT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR))g_cb)(
                VHANDLE, msg, g_cbInst, d1, d2);
        break;
    case CALLBACK_WINDOW:
        if (g_cb) PostMessage((HWND)g_cb, msg, (WPARAM)VHANDLE, (LPARAM)d1);
        break;
    case CALLBACK_THREAD:
        if (g_cb) PostThreadMessage((DWORD)g_cb, msg, (WPARAM)VHANDLE, (LPARAM)d1);
        break;
    case CALLBACK_EVENT:
        if (g_cb) SetEvent((HANDLE)g_cb);
        break;
    }
}

/* ======================================================================== */
/*  override midiOut*                                                       */
/* ======================================================================== */
UINT WINAPI pxy_midiOutGetNumDevs(void)
{
    return vid() + 1;
}

MMRESULT WINAPI pxy_midiOutGetDevCapsA(UINT_PTR id, LPMIDIOUTCAPSA pmoc, UINT cb)
{
    ensure_init();
    if (is_virtual_idorhandle(id)) {
        MIDIOUTCAPSA c;
        if (!pmoc) return MMSYSERR_INVALPARAM;
        memset(&c, 0, sizeof c);
        c.wMid = 1;                       /* MM_MICROSOFT */
        c.wPid = 10;                      /* MM_MPU401_MIDIOUT */
        c.vDriverVersion = 0x0100;
        lstrcpynA(c.szPname, g_nameA, MAXPNAMELEN);
        c.wTechnology = MOD_MIDIPORT;
        c.wChannelMask = 0xFFFF;
        memcpy(pmoc, &c, cb < sizeof c ? cb : sizeof c);
        return MMSYSERR_NOERROR;
    }
    return R.capsA ? R.capsA(id, pmoc, cb) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutGetDevCapsW(UINT_PTR id, LPMIDIOUTCAPSW pmoc, UINT cb)
{
    ensure_init();
    if (is_virtual_idorhandle(id)) {
        MIDIOUTCAPSW c;
        if (!pmoc) return MMSYSERR_INVALPARAM;
        memset(&c, 0, sizeof c);
        c.wMid = 1;
        c.wPid = 10;
        c.vDriverVersion = 0x0100;
        lstrcpynW(c.szPname, g_nameW, MAXPNAMELEN);
        c.wTechnology = MOD_MIDIPORT;
        c.wChannelMask = 0xFFFF;
        memcpy(pmoc, &c, cb < sizeof c ? cb : sizeof c);
        return MMSYSERR_NOERROR;
    }
    return R.capsW ? R.capsW(id, pmoc, cb) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutOpen(LPHMIDIOUT phmo, UINT dev, DWORD_PTR cb, DWORD_PTR inst, DWORD flags)
{
    ensure_init();
    if (dev != (UINT)-1 && dev == vid()) {
        WSADATA w;
        if (!phmo) return MMSYSERR_INVALPARAM;
        if (g_opened) return MMSYSERR_ALLOCATED;
        EnterCriticalSection(&g_cs);
        if (!g_wsa && WSAStartup(MAKEWORD(2, 2), &w) == 0) g_wsa = 1;
        g_lastTry = 0;
        LeaveCriticalSection(&g_cs);
        g_opened = 1;
        g_cb = cb; g_cbInst = inst; g_cbFlags = flags;
        *phmo = VHANDLE;
        tx1('O');
        fire_cb(MOM_OPEN, 0, 0);
        return MMSYSERR_NOERROR;
    }
    return R.open ? R.open(phmo, dev, cb, inst, flags) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutClose(HMIDIOUT h)
{
    if (is_vh((UINT_PTR)h)) {
        tx1('C');
        g_opened = 0;
        fire_cb(MOM_CLOSE, 0, 0);
        EnterCriticalSection(&g_cs);
        if (g_sock != INVALID_SOCKET) { closesocket(g_sock); g_sock = INVALID_SOCKET; }
        LeaveCriticalSection(&g_cs);
        return MMSYSERR_NOERROR;
    }
    ensure_init();
    return R.close ? R.close(h) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutShortMsg(HMIDIOUT h, DWORD msg)
{
    if (is_vh((UINT_PTR)h)) {
        unsigned char b[5];
        b[0] = 'S';
        b[1] = (unsigned char)(msg & 0xFF);
        b[2] = (unsigned char)((msg >> 8) & 0xFF);
        b[3] = (unsigned char)((msg >> 16) & 0xFF);
        b[4] = (unsigned char)((msg >> 24) & 0xFF);
        tx(b, 5, NULL, 0);
        return MMSYSERR_NOERROR;
    }
    ensure_init();
    return R.shortmsg ? R.shortmsg(h, msg) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutLongMsg(HMIDIOUT h, LPMIDIHDR pmh, UINT cb)
{
    if (is_vh((UINT_PTR)h)) {
        unsigned char hdr[5];
        DWORD n;
        if (!pmh) return MMSYSERR_INVALPARAM;
        n = pmh->dwBufferLength;
        hdr[0] = 'L';
        hdr[1] = (unsigned char)(n & 0xFF);
        hdr[2] = (unsigned char)((n >> 8) & 0xFF);
        hdr[3] = (unsigned char)((n >> 16) & 0xFF);
        hdr[4] = (unsigned char)((n >> 24) & 0xFF);
        tx(hdr, 5, (const unsigned char *)pmh->lpData, (int)n);
        pmh->dwFlags |= MHDR_DONE;
        pmh->dwFlags &= ~MHDR_INQUEUE;
        fire_cb(MOM_DONE, (DWORD_PTR)pmh, 0);
        return MMSYSERR_NOERROR;
    }
    ensure_init();
    return R.longmsg ? R.longmsg(h, pmh, cb) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutPrepareHeader(HMIDIOUT h, LPMIDIHDR pmh, UINT cb)
{
    if (is_vh((UINT_PTR)h)) {
        if (!pmh) return MMSYSERR_INVALPARAM;
        pmh->dwFlags |= MHDR_PREPARED;
        return MMSYSERR_NOERROR;
    }
    ensure_init();
    return R.prep ? R.prep(h, pmh, cb) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutUnprepareHeader(HMIDIOUT h, LPMIDIHDR pmh, UINT cb)
{
    if (is_vh((UINT_PTR)h)) {
        if (!pmh) return MMSYSERR_INVALPARAM;
        pmh->dwFlags &= ~MHDR_PREPARED;
        return MMSYSERR_NOERROR;
    }
    ensure_init();
    return R.unprep ? R.unprep(h, pmh, cb) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutReset(HMIDIOUT h)
{
    if (is_vh((UINT_PTR)h)) {
        tx1('R');
        return MMSYSERR_NOERROR;
    }
    ensure_init();
    return R.reset ? R.reset(h) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutGetID(HMIDIOUT h, LPUINT pid)
{
    if (is_vh((UINT_PTR)h)) {
        if (!pid) return MMSYSERR_INVALPARAM;
        *pid = vid();
        return MMSYSERR_NOERROR;
    }
    ensure_init();
    return R.getid ? R.getid(h, pid) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutMessage(HMIDIOUT h, UINT msg, DWORD_PTR d1, DWORD_PTR d2)
{
    if (is_vh((UINT_PTR)h)) return MMSYSERR_NOERROR;
    ensure_init();
    return R.msg ? R.msg(h, msg, d1, d2) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutGetVolume(HMIDIOUT h, LPDWORD pv)
{
    ensure_init();
    if (is_virtual_idorhandle((UINT_PTR)h)) {
        if (pv) *pv = 0xFFFFFFFF;
        return MMSYSERR_NOERROR;
    }
    return R.getvol ? R.getvol(h, pv) : MMSYSERR_NODRIVER;
}

MMRESULT WINAPI pxy_midiOutSetVolume(HMIDIOUT h, DWORD v)
{
    ensure_init();
    if (is_virtual_idorhandle((UINT_PTR)h)) return MMSYSERR_NOERROR;
    return R.setvol ? R.setvol(h, v) : MMSYSERR_NODRIVER;
}

/* ======================================================================== */
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
    }
    return TRUE;
}
