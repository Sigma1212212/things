/*
 * ASM3D - a3_audio_wasapi.c
 * Windows audio output: WASAPI shared mode, event driven, 32-bit float
 * stereo (Windows converts to the device format). ~20 ms latency.
 */
#include "a3_audio_backend.h"
#include "../core/a3_log.h"
#include "../core/a3_string.h"
#include "../core/a3_memory.h"

#if A3_PLATFORM_WINDOWS

#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

/* GUIDs defined locally so no extra import libraries are needed */
DEFINE_GUID(A3_CLSID_MMDeviceEnumerator, 0xBCDE0395, 0xE52F, 0x467C, 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E);
DEFINE_GUID(A3_IID_IMMDeviceEnumerator, 0xA95664D2, 0x9614, 0x4F35, 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6);
DEFINE_GUID(A3_IID_IAudioClient, 0x1CB9AD4C, 0xDBFA, 0x4C32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2);
DEFINE_GUID(A3_IID_IAudioRenderClient, 0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2);
DEFINE_GUID(A3_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#  define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#  define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

static struct {
    HANDLE thread, ready, event;
    volatile LONG quit;
    b32 ok;
    u32 rate;
    A3AudioRenderFn render;
    char name[96];
} g_wa;

static DWORD WINAPI audio_thread(LPVOID p) {
    A3_UNUSED(p);
    IMMDeviceEnumerator *en = 0;
    IMMDevice *dev = 0;
    IAudioClient *client = 0;
    IAudioRenderClient *rc = 0;
    WAVEFORMATEX *mix = 0;
    f32 *buf = 0;
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    b32 com = SUCCEEDED(hr);
    hr = CoCreateInstance(&A3_CLSID_MMDeviceEnumerator, 0, CLSCTX_ALL, &A3_IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) goto fail;
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (FAILED(hr)) goto fail;
    hr = IMMDevice_Activate(dev, &A3_IID_IAudioClient, CLSCTX_ALL, 0, (void **)&client);
    if (FAILED(hr)) goto fail;
    hr = IAudioClient_GetMixFormat(client, &mix);
    if (FAILED(hr)) goto fail;
    g_wa.rate = mix->nSamplesPerSec;
    WAVEFORMATEXTENSIBLE fmt;
    a3_zero_struct(&fmt);
    fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fmt.Format.nChannels = 2;
    fmt.Format.nSamplesPerSec = g_wa.rate;
    fmt.Format.wBitsPerSample = 32;
    fmt.Format.nBlockAlign = 8;
    fmt.Format.nAvgBytesPerSec = g_wa.rate * 8;
    fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    fmt.Samples.wValidBitsPerSample = 32;
    fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    fmt.SubFormat = A3_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 200000 /* 20 ms in 100 ns units */, 0, (WAVEFORMATEX *)&fmt, 0);
    if (FAILED(hr)) goto fail;
    g_wa.event = CreateEventW(0, FALSE, FALSE, 0);
    if (!g_wa.event || FAILED(IAudioClient_SetEventHandle(client, g_wa.event))) goto fail;
    UINT32 frames = 0;
    if (FAILED(IAudioClient_GetBufferSize(client, &frames))) goto fail;
    if (FAILED(IAudioClient_GetService(client, &A3_IID_IAudioRenderClient, (void **)&rc))) goto fail;
    buf = (f32 *)a3_malloc(sizeof(f32) * 2 * frames, A3_MEM_AUDIO);
    if (!buf) goto fail;
    if (FAILED(IAudioClient_Start(client))) goto fail;
    a3_strcpy(g_wa.name, sizeof(g_wa.name), "WASAPI (default device)");
    g_wa.ok = 1;
    SetEvent(g_wa.ready);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (!g_wa.quit) {
        WaitForSingleObject(g_wa.event, 100);
        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(client, &padding))) break; /* device removed */
        UINT32 avail = frames - padding;
        if (!avail) continue;
        BYTE *dst = 0;
        if (FAILED(IAudioRenderClient_GetBuffer(rc, avail, &dst))) continue;
        g_wa.render(buf, avail);
        a3_memcpy(dst, buf, sizeof(f32) * 2 * avail);
        IAudioRenderClient_ReleaseBuffer(rc, avail, 0);
    }
    IAudioClient_Stop(client);
    goto cleanup;
fail:
    g_wa.ok = 0;
    SetEvent(g_wa.ready);
cleanup:
    a3_free(buf);
    if (rc) IAudioRenderClient_Release(rc);
    if (mix) CoTaskMemFree(mix);
    if (client) IAudioClient_Release(client);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (g_wa.event) { CloseHandle(g_wa.event); g_wa.event = 0; }
    if (com) CoUninitialize();
    return 0;
}

b32 a3_audio_backend_open(u32 *rate, A3AudioRenderFn render, char *name, usize name_cap) {
    a3_zero_struct(&g_wa);
    g_wa.render = render;
    g_wa.ready = CreateEventW(0, TRUE, FALSE, 0);
    g_wa.thread = CreateThread(0, 0, audio_thread, 0, 0, 0);
    if (!g_wa.thread) { CloseHandle(g_wa.ready); return 0; }
    WaitForSingleObject(g_wa.ready, 5000);
    CloseHandle(g_wa.ready);
    g_wa.ready = 0;
    if (!g_wa.ok) {
        WaitForSingleObject(g_wa.thread, 2000);
        CloseHandle(g_wa.thread);
        g_wa.thread = 0;
        return 0;
    }
    *rate = g_wa.rate;
    a3_strcpy(name, name_cap, g_wa.name);
    return 1;
}

void a3_audio_backend_close(void) {
    if (!g_wa.thread) return;
    InterlockedExchange(&g_wa.quit, 1);
    if (g_wa.event) SetEvent(g_wa.event);
    WaitForSingleObject(g_wa.thread, 2000);
    CloseHandle(g_wa.thread);
    g_wa.thread = 0;
}

#endif /* A3_PLATFORM_WINDOWS */
