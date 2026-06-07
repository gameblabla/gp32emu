#include "gp32_win64_audio.h"
#include "audio/gp32_audio_resampler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS 1
#endif
#ifndef INITGUID
#define INITGUID
#endif
#include <windows.h>
#include <mmsystem.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WAVE_FORMAT_IEEE_FLOAT
#define WAVE_FORMAT_IEEE_FLOAT 0x0003
#endif

#define GP32_AUDIO_CHANNELS 2u
#define GP32_AUDIO_BUFFERS 4u
#define GP32_AUDIO_WAVE_FRAMES 1024u
#define GP32_AUDIO_RING_DEFAULT 8192u
#define GP32_AUDIO_MAX_LATENCY_FRAMES 32768u
#define GP32_AUDIO_START_FRAMES 4096u
#define GP32_AUDIO_MIN_WRITE_FRAMES 1024u
#define GP32_AUDIO_GAP_FADE_FRAMES 256u

#ifndef GP32EMU_ENABLE_THREADS
#define GP32EMU_ENABLE_THREADS 1
#endif

typedef enum audio_kind { AUDIO_NONE = 0, AUDIO_WAVEOUT, AUDIO_WASAPI } audio_kind_t;

struct gp32_win64_audio {
    audio_kind_t kind;
    gp32_win64_audio_mode_t requested_mode;
    uint32_t sample_rate;
    int16_t *queue;
    uint32_t read_frame;
    uint32_t frame_count;
    uint32_t frame_cap;
    int16_t *tmp;
    uint32_t tmp_cap_frames;
    gp32_audio_resampler_t resampler;
    int underrun;
    int playback_started;
    int16_t last_ring_l;
    int16_t last_ring_r;
    int have_last_ring;
    char backend_name[48];
    char error[192];
    HWAVEOUT wave;
    WAVEHDR hdr[GP32_AUDIO_BUFFERS];
    int16_t *wave_buf[GP32_AUDIO_BUFFERS];
    int com_initialized;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    WAVEFORMATEX *mixfmt;
    UINT32 wasapi_buffer_frames;
    int wasapi_started;
#if GP32EMU_ENABLE_THREADS
    CRITICAL_SECTION lock;
    int lock_ready;
    HANDLE pump_thread;
    HANDLE pump_event;
    volatile LONG pump_stop;
    int threaded_pump;
#endif
};

static void set_error(gp32_win64_audio_t *a, const char *msg) {
    if (!a) return;
    snprintf(a->error, sizeof(a->error), "%s", msg ? msg : "audio error");
}

#if GP32EMU_ENABLE_THREADS
static void audio_lock(gp32_win64_audio_t *a) { if (a && a->lock_ready) EnterCriticalSection(&a->lock); }
static void audio_unlock(gp32_win64_audio_t *a) { if (a && a->lock_ready) LeaveCriticalSection(&a->lock); }
#else
static void audio_lock(gp32_win64_audio_t *a) { (void)a; }
static void audio_unlock(gp32_win64_audio_t *a) { (void)a; }
#endif

static int ensure_queue(gp32_win64_audio_t *a, uint32_t extra) {
    if (!a) return 0;
    if (extra <= a->frame_cap - a->frame_count) return 1;
    uint32_t old_cap = a->frame_cap;
    uint32_t new_cap = old_cap ? old_cap : GP32_AUDIO_RING_DEFAULT;
    while (new_cap - a->frame_count < extra) {
        if (new_cap > UINT32_MAX / 2u) return 0;
        new_cap *= 2u;
    }
    int16_t *n = (int16_t *)malloc((size_t)new_cap * GP32_AUDIO_CHANNELS * sizeof(int16_t));
    if (!n) return 0;
    for (uint32_t i = 0; i < a->frame_count; ++i) {
        uint32_t src = old_cap ? ((a->read_frame + i) % old_cap) : 0u;
        n[(size_t)i * 2u + 0u] = a->queue[(size_t)src * 2u + 0u];
        n[(size_t)i * 2u + 1u] = a->queue[(size_t)src * 2u + 1u];
    }
    free(a->queue);
    a->queue = n;
    a->frame_cap = new_cap;
    a->read_frame = 0;
    return 1;
}


static int ensure_tmp(gp32_win64_audio_t *a, uint32_t frames) {
    if (frames <= a->tmp_cap_frames) return 1;
    int16_t *n = (int16_t *)realloc(a->tmp, (size_t)frames * GP32_AUDIO_CHANNELS * sizeof(int16_t));
    if (!n) return 0;
    a->tmp = n;
    a->tmp_cap_frames = frames;
    return 1;
}

static int32_t queue_rate_adjust_ppm(uint32_t queued_frames) {
    const int32_t target = 4096;
    const int32_t span = 4096;
    const int32_t max_slowdown = 35000;
    const int32_t max_speedup = -25000;
    int32_t err = target - (int32_t)queued_frames;
    if (err > span) err = span;
    if (err < -span) err = -span;
    int32_t ppm = (err * max_slowdown) / span;
    if (ppm < max_speedup) ppm = max_speedup;
    if (ppm > max_slowdown) ppm = max_slowdown;
    return ppm;
}

static void drop_oldest(gp32_win64_audio_t *a, uint32_t frames) {
    if (!a || !frames || !a->frame_cap) return;
    if (frames >= a->frame_count) { a->read_frame = 0; a->frame_count = 0; return; }
    a->read_frame = (a->read_frame + frames) % a->frame_cap;
    a->frame_count -= frames;
}

static void ring_write_bulk_unlocked(gp32_win64_audio_t *a, const int16_t *src, uint32_t frames) {
    if (!a || !src || !frames) return;
    uint32_t w = (a->read_frame + a->frame_count) % a->frame_cap;
    uint32_t first = a->frame_cap - w;
    if (first > frames) first = frames;
    memcpy(a->queue + (size_t)w * 2u, src, (size_t)first * 2u * sizeof(int16_t));
    if (first < frames) {
        memcpy(a->queue, src + (size_t)first * 2u, (size_t)(frames - first) * 2u * sizeof(int16_t));
    }
    a->frame_count += frames;
}

static void fill_gap_tail(gp32_win64_audio_t *a, int16_t *out, uint32_t frames) {
    if (!a || !out || !frames) return;
    int32_t l0 = a->have_last_ring ? a->last_ring_l : 0;
    int32_t r0 = a->have_last_ring ? a->last_ring_r : 0;
    uint32_t fade = frames < GP32_AUDIO_GAP_FADE_FRAMES ? frames : GP32_AUDIO_GAP_FADE_FRAMES;
    for (uint32_t i = 0; i < fade; ++i) {
        int32_t num = (int32_t)(fade - i);
        out[(size_t)i * 2u + 0u] = (int16_t)((l0 * num) / (int32_t)fade);
        out[(size_t)i * 2u + 1u] = (int16_t)((r0 * num) / (int32_t)fade);
    }
    if (fade < frames) memset(out + (size_t)fade * 2u, 0, (size_t)(frames - fade) * 2u * sizeof(int16_t));
    a->last_ring_l = 0;
    a->last_ring_r = 0;
    a->have_last_ring = 1;
}

static uint32_t ring_read_unlocked(gp32_win64_audio_t *a, int16_t *out, uint32_t frames) {
    uint32_t copied = 0;
    if (!a || !out || !frames) return 0;
    uint32_t n = a->frame_count < frames ? a->frame_count : frames;
    while (copied < n) {
        uint32_t contiguous = a->frame_cap - a->read_frame;
        uint32_t chunk = n - copied;
        if (chunk > contiguous) chunk = contiguous;
        memcpy(out + (size_t)copied * 2u, a->queue + (size_t)a->read_frame * 2u, (size_t)chunk * 2u * sizeof(int16_t));
        a->read_frame = (a->read_frame + chunk) % a->frame_cap;
        a->frame_count -= chunk;
        copied += chunk;
    }
    if (copied) {
        a->last_ring_l = out[(size_t)(copied - 1u) * 2u + 0u];
        a->last_ring_r = out[(size_t)(copied - 1u) * 2u + 1u];
        a->have_last_ring = 1;
    }
    if (copied < frames) {
        a->underrun = 1;
        fill_gap_tail(a, out + (size_t)copied * 2u, frames - copied);
    }
    return copied;
}

static uint32_t ring_read(gp32_win64_audio_t *a, int16_t *out, uint32_t frames) {
    audio_lock(a);
    uint32_t n = ring_read_unlocked(a, out, frames);
    audio_unlock(a);
    return n;
}


int gp32_win64_audio_submit(gp32_win64_audio_t *a, const gp32_audio_desc_t *audio) {
    if (!a || !audio) return -1;
    if (!audio->samples_s16_interleaved || audio->frame_count == 0) return 0;
    uint32_t src_rate = audio->sample_rate_hz ? audio->sample_rate_hz : a->sample_rate;
    uint32_t dst_rate = a->sample_rate ? a->sample_rate : src_rate;
    if (!src_rate || !dst_rate) return 0;
    if (audio->frame_count > UINT32_MAX / 2u) { set_error(a, "audio chunk too large"); return -1; }

    audio_lock(a);
    int had_underrun = a->underrun;
    a->underrun = 0;
    uint32_t queued_frames = a->frame_count;
    audio_unlock(a);
    if (had_underrun) {
        gp32_audio_resampler_mark_gap(&a->resampler, dst_rate);
    }
    int32_t adjust_ppm = queue_rate_adjust_ppm(queued_frames);
    size_t max_out = gp32_audio_resampler_max_output_frames(&a->resampler,
                                                            (size_t)audio->frame_count,
                                                            src_rate,
                                                            dst_rate,
                                                            adjust_ppm);
    if (max_out > UINT32_MAX) { set_error(a, "resampled audio chunk too large"); return -1; }
    if (!ensure_tmp(a, (uint32_t)max_out)) { set_error(a, "audio resample buffer allocation failed"); return -1; }
    size_t out_sz = gp32_audio_resampler_process(&a->resampler,
                                                 audio->samples_s16_interleaved,
                                                 (size_t)audio->frame_count,
                                                 src_rate,
                                                 dst_rate,
                                                 adjust_ppm,
                                                 a->tmp,
                                                 max_out);
    if (!out_sz) return 0;
    if (out_sz > UINT32_MAX) { set_error(a, "resampled audio chunk too large"); return -1; }
    uint32_t out_frames = (uint32_t)out_sz;
    audio_lock(a);
    if (a->frame_count + out_frames > GP32_AUDIO_MAX_LATENCY_FRAMES) {
        drop_oldest(a, a->frame_count + out_frames - GP32_AUDIO_MAX_LATENCY_FRAMES);
        gp32_audio_resampler_mark_gap(&a->resampler, dst_rate);
    }
    if (!ensure_queue(a, out_frames)) { audio_unlock(a); set_error(a, "audio queue allocation failed"); return -1; }
    ring_write_bulk_unlocked(a, a->tmp, out_frames);
    audio_unlock(a);
#if GP32EMU_ENABLE_THREADS
    if (a->pump_event) SetEvent(a->pump_event);
#endif
    return 0;
}

static int waveout_open(gp32_win64_audio_t *a) {
    WAVEFORMATEX fmt; memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = a->sample_rate;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8u);
    fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
    MMRESULT mm = waveOutOpen(&a->wave, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL);
    if (mm != MMSYSERR_NOERROR) { set_error(a, "waveOutOpen failed"); return 0; }
    for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
        a->wave_buf[i] = (int16_t *)calloc((size_t)GP32_AUDIO_WAVE_FRAMES * 2u, sizeof(int16_t));
        if (!a->wave_buf[i]) return 0;
        memset(&a->hdr[i], 0, sizeof(a->hdr[i]));
        a->hdr[i].lpData = (LPSTR)a->wave_buf[i];
        a->hdr[i].dwBufferLength = (DWORD)(GP32_AUDIO_WAVE_FRAMES * 2u * sizeof(int16_t));
    }
    a->kind = AUDIO_WAVEOUT;
    snprintf(a->backend_name, sizeof(a->backend_name), "waveOut %u Hz", a->sample_rate);
    return 1;
}

static int waveout_pump(gp32_win64_audio_t *a) {
    int wrote = 0;
    for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
        WAVEHDR *h = &a->hdr[i];
        if ((h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE)) continue;
        if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(a->wave, h, sizeof(*h));
        memset(h, 0, sizeof(*h));
        h->lpData = (LPSTR)a->wave_buf[i];
        h->dwBufferLength = (DWORD)(GP32_AUDIO_WAVE_FRAMES * 2u * sizeof(int16_t));
        ring_read(a, a->wave_buf[i], GP32_AUDIO_WAVE_FRAMES);
        if (waveOutPrepareHeader(a->wave, h, sizeof(*h)) != MMSYSERR_NOERROR) return -1;
        if (waveOutWrite(a->wave, h, sizeof(*h)) != MMSYSERR_NOERROR) return -1;
        if (++wrote >= 2) break;
    }
    return 0;
}

static int is_extensible_subtype(const WAVEFORMATEX *fmt, const GUID *sub) {
    if (!fmt || fmt->wFormatTag != WAVE_FORMAT_EXTENSIBLE || fmt->cbSize < 22) return 0;
    const WAVEFORMATEXTENSIBLE *ex = (const WAVEFORMATEXTENSIBLE *)fmt;
    return IsEqualGUID(&ex->SubFormat, sub) != 0;
}

static WAVEFORMATEX *make_exclusive_pcm(uint32_t rate) {
    WAVEFORMATEXTENSIBLE *ex = (WAVEFORMATEXTENSIBLE *)CoTaskMemAlloc(sizeof(*ex));
    if (!ex) return NULL;
    memset(ex, 0, sizeof(*ex));
    ex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ex->Format.nChannels = 2;
    ex->Format.nSamplesPerSec = rate;
    ex->Format.wBitsPerSample = 16;
    ex->Format.nBlockAlign = (WORD)(ex->Format.nChannels * ex->Format.wBitsPerSample / 8u);
    ex->Format.nAvgBytesPerSec = ex->Format.nSamplesPerSec * ex->Format.nBlockAlign;
    ex->Format.cbSize = 22;
    ex->Samples.wValidBitsPerSample = 16;
    ex->dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    ex->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return &ex->Format;
}

static int wasapi_open(gp32_win64_audio_t *a, int exclusive) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) a->com_initialized = 1;
    else if (hr != RPC_E_CHANGED_MODE) { set_error(a, "CoInitializeEx failed"); return 0; }
    IMMDeviceEnumerator *enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&enumerator);
    if (FAILED(hr) || !enumerator) { set_error(a, "MMDeviceEnumerator creation failed"); return 0; }
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &a->device);
    IMMDeviceEnumerator_Release(enumerator);
    if (FAILED(hr) || !a->device) { set_error(a, "default audio endpoint unavailable"); return 0; }
    hr = IMMDevice_Activate(a->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&a->client);
    if (FAILED(hr) || !a->client) { set_error(a, "IAudioClient activation failed"); return 0; }
    if (exclusive) {
        uint32_t requested_rate = a->sample_rate ? a->sample_rate : 44100u;
        uint32_t preferred_rate = requested_rate;
        WAVEFORMATEX *device_mix = NULL;
        if (SUCCEEDED(IAudioClient_GetMixFormat(a->client, &device_mix)) && device_mix) {
            if (device_mix->nSamplesPerSec) preferred_rate = device_mix->nSamplesPerSec;
            CoTaskMemFree(device_mix);
        }
        a->mixfmt = make_exclusive_pcm(preferred_rate);
        if (!a->mixfmt) return 0;
        hr = IAudioClient_IsFormatSupported(a->client, AUDCLNT_SHAREMODE_EXCLUSIVE, a->mixfmt, NULL);
        if (FAILED(hr) && preferred_rate != requested_rate) {
            CoTaskMemFree(a->mixfmt);
            a->mixfmt = make_exclusive_pcm(requested_rate);
            if (!a->mixfmt) return 0;
            hr = IAudioClient_IsFormatSupported(a->client, AUDCLNT_SHAREMODE_EXCLUSIVE, a->mixfmt, NULL);
        }
        if (FAILED(hr)) { set_error(a, "WASAPI exclusive PCM 16-bit stereo format unsupported"); return 0; }
        if (a->mixfmt->nSamplesPerSec) a->sample_rate = a->mixfmt->nSamplesPerSec;
        REFERENCE_TIME dur = 500000; /* 50 ms */
        hr = IAudioClient_Initialize(a->client, AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur, dur, a->mixfmt, NULL);
        if (FAILED(hr)) { set_error(a, "WASAPI exclusive Initialize failed"); return 0; }
        snprintf(a->backend_name, sizeof(a->backend_name), "WASAPI exclusive %u Hz", a->sample_rate);
    } else {
        hr = IAudioClient_GetMixFormat(a->client, &a->mixfmt);
        if (FAILED(hr) || !a->mixfmt) { set_error(a, "WASAPI GetMixFormat failed"); return 0; }
        if (a->mixfmt->nSamplesPerSec) {
            /*
             * Shared-mode WASAPI consumes frames at the endpoint mix rate,
             * commonly 48000 Hz.  The first Win64 backend kept the internal
             * queue at the requested 44100 Hz while writing into a 48000 Hz
             * shared stream, so audio played at the wrong pitch/tempo.
             */
            a->sample_rate = a->mixfmt->nSamplesPerSec;
        }
        REFERENCE_TIME dur = 1000000; /* 100 ms */
        hr = IAudioClient_Initialize(a->client, AUDCLNT_SHAREMODE_SHARED, 0, dur, 0, a->mixfmt, NULL);
        if (FAILED(hr)) { set_error(a, "WASAPI shared Initialize failed"); return 0; }
        snprintf(a->backend_name, sizeof(a->backend_name), "WASAPI shared %u Hz", a->sample_rate);
    }
    hr = IAudioClient_GetBufferSize(a->client, &a->wasapi_buffer_frames);
    if (FAILED(hr)) return 0;
    hr = IAudioClient_GetService(a->client, &IID_IAudioRenderClient, (void **)&a->render);
    if (FAILED(hr) || !a->render) return 0;
    a->wasapi_started = 0;
    a->kind = AUDIO_WASAPI;
    return 1;
}

static uint32_t fill_wasapi_buffer(gp32_win64_audio_t *a, BYTE *dst, UINT32 frames) {
    if (!a || !dst || !a->mixfmt || !frames) return 0;
    WAVEFORMATEX *fmt = a->mixfmt;
    int channels = fmt->nChannels ? (int)fmt->nChannels : 2;
    int is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || is_extensible_subtype(fmt, &KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    int is_pcm = fmt->wFormatTag == WAVE_FORMAT_PCM || is_extensible_subtype(fmt, &KSDATAFORMAT_SUBTYPE_PCM);
    int bits = fmt->wBitsPerSample;
    int16_t tmp[4096 * 2];
    UINT32 done = 0;
    while (done < frames) {
        UINT32 chunk = frames - done;
        if (chunk > 4096u) chunk = 4096u;
        uint32_t got = ring_read(a, tmp, chunk);
        BYTE *base = dst + (size_t)done * fmt->nBlockAlign;
        if (is_float && bits == 32) {
            float *out = (float *)base;
            for (UINT32 i = 0; i < chunk; ++i) {
                float l = (float)tmp[(size_t)i * 2u + 0u] / 32768.0f;
                float r = (float)tmp[(size_t)i * 2u + 1u] / 32768.0f;
                for (int c = 0; c < channels; ++c) *out++ = (c & 1) ? r : l;
            }
        } else if (is_pcm && bits == 16) {
            int16_t *out = (int16_t *)base;
            for (UINT32 i = 0; i < chunk; ++i) {
                int16_t l = tmp[(size_t)i * 2u + 0u], r = tmp[(size_t)i * 2u + 1u];
                for (int c = 0; c < channels; ++c) *out++ = (c & 1) ? r : l;
            }
        } else {
            memset(base, 0, (size_t)chunk * fmt->nBlockAlign);
        }
        (void)got;
        done += chunk;
    }
    return frames;
}

static int wasapi_pump(gp32_win64_audio_t *a) {
    if (!a || !a->client || !a->render) return -1;

    audio_lock(a);
    uint32_t queued = a->frame_count;
    int started = a->playback_started;
    audio_unlock(a);

    if (!started && queued < GP32_AUDIO_START_FRAMES) return 0;

    UINT32 padding = 0;
    HRESULT hr = IAudioClient_GetCurrentPadding(a->client, &padding);
    if (FAILED(hr)) return -1;

    if (started && queued == 0u && padding <= 256u) {
        if (a->wasapi_started) IAudioClient_Stop(a->client);
        IAudioClient_Reset(a->client);
        a->wasapi_started = 0;
        audio_lock(a);
        a->playback_started = 0;
        a->underrun = 1;
        audio_unlock(a);
        gp32_audio_resampler_mark_gap(&a->resampler, a->sample_rate);
        return 0;
    }

    UINT32 avail = a->wasapi_buffer_frames > padding ? a->wasapi_buffer_frames - padding : 0;
    if (avail < 256u) return 0;
    if (avail > 4096u) avail = 4096u;
    if (!started && avail > queued) avail = queued;
    if (avail < 256u) return 0;

    BYTE *dst = NULL;
    hr = IAudioRenderClient_GetBuffer(a->render, avail, &dst);
    if (FAILED(hr) || !dst) return -1;
    fill_wasapi_buffer(a, dst, avail);
    hr = IAudioRenderClient_ReleaseBuffer(a->render, avail, 0);
    if (FAILED(hr)) return -1;

    if (!started) {
        hr = IAudioClient_Start(a->client);
        if (FAILED(hr)) return -1;
        a->wasapi_started = 1;
        audio_lock(a);
        a->playback_started = 1;
        audio_unlock(a);
    }
    return 0;
}


static void wasapi_cleanup_partial(gp32_win64_audio_t *a) {
    if (!a) return;
    if (a->client && a->wasapi_started) IAudioClient_Stop(a->client);
    if (a->render) { IAudioRenderClient_Release(a->render); a->render = NULL; }
    if (a->client) { IAudioClient_Release(a->client); a->client = NULL; }
    if (a->device) { IMMDevice_Release(a->device); a->device = NULL; }
    if (a->mixfmt) { CoTaskMemFree(a->mixfmt); a->mixfmt = NULL; }
    if (a->com_initialized) { CoUninitialize(); a->com_initialized = 0; }
    a->wasapi_started = 0;
}


#if GP32EMU_ENABLE_THREADS
static int gp32_win64_audio_pump_backend(gp32_win64_audio_t *a);
static DWORD WINAPI audio_pump_thread_proc(LPVOID opaque) {
    gp32_win64_audio_t *a = (gp32_win64_audio_t *)opaque;
    DWORD task_index = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
    while (InterlockedCompareExchange(&a->pump_stop, 0, 0) == 0) {
        gp32_win64_audio_pump_backend(a);
        if (a->pump_event) WaitForSingleObject(a->pump_event, 2);
        else Sleep(2);
    }
    if (avrt) AvRevertMmThreadCharacteristics(avrt);
    return 0;
}

static int audio_start_thread(gp32_win64_audio_t *a) {
    if (!a) return 0;
    a->pump_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    a->pump_thread = CreateThread(NULL, 0, audio_pump_thread_proc, a, 0, NULL);
    if (!a->pump_thread) {
        if (a->pump_event) { CloseHandle(a->pump_event); a->pump_event = NULL; }
        set_error(a, "CreateThread failed for audio pump");
        return 0;
    }
    a->threaded_pump = 1;
    return 1;
}

static void audio_stop_thread(gp32_win64_audio_t *a) {
    if (!a || !a->threaded_pump) return;
    InterlockedExchange(&a->pump_stop, 1);
    if (a->pump_event) SetEvent(a->pump_event);
    WaitForSingleObject(a->pump_thread, INFINITE);
    CloseHandle(a->pump_thread);
    if (a->pump_event) CloseHandle(a->pump_event);
    a->pump_thread = NULL;
    a->pump_event = NULL;
    a->threaded_pump = 0;
}
#endif

int gp32_win64_audio_pump(gp32_win64_audio_t *a) {
#if GP32EMU_ENABLE_THREADS
    if (a && a->threaded_pump) { if (a->pump_event) SetEvent(a->pump_event); return 0; }
#endif
    return gp32_win64_audio_pump_backend(a);
}

gp32_win64_audio_t *gp32_win64_audio_create(gp32_win64_audio_mode_t mode, uint32_t sample_rate_hz) {
    gp32_win64_audio_t *a = (gp32_win64_audio_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;
#if GP32EMU_ENABLE_THREADS
    InitializeCriticalSection(&a->lock);
    a->lock_ready = 1;
#endif
    a->sample_rate = sample_rate_hz ? sample_rate_hz : 44100u;
    gp32_audio_resampler_init(&a->resampler);
    a->requested_mode = mode;
    if (mode == GP32_WIN64_AUDIO_WASAPI_SHARED || mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE) {
        if (wasapi_open(a, mode == GP32_WIN64_AUDIO_WASAPI_EXCLUSIVE)) {
#if GP32EMU_ENABLE_THREADS
            audio_start_thread(a);
#endif
            return a;
        }
    } else if (waveout_open(a)) {
#if GP32EMU_ENABLE_THREADS
        audio_start_thread(a);
#endif
        return a;
    }
    if (a->kind == AUDIO_NONE && mode != GP32_WIN64_AUDIO_WAVEOUT) {
        char wasapi_error[sizeof(a->error)];
        snprintf(wasapi_error, sizeof(wasapi_error), "%s", a->error[0] ? a->error : "WASAPI unavailable");
        wasapi_cleanup_partial(a);
        if (waveout_open(a)) { snprintf(a->error, sizeof(a->error), "WASAPI failed, using waveOut: %s", wasapi_error);
#if GP32EMU_ENABLE_THREADS
            audio_start_thread(a);
#endif
            return a; }
    }
    gp32_win64_audio_destroy(a);
    return NULL;
}

static int gp32_win64_audio_pump_backend(gp32_win64_audio_t *a) {
    if (!a) return -1;
    if (a->kind == AUDIO_WAVEOUT) return waveout_pump(a);
    if (a->kind == AUDIO_WASAPI) return wasapi_pump(a);
    return 0;
}

void gp32_win64_audio_destroy(gp32_win64_audio_t *a) {
    if (!a) return;
#if GP32EMU_ENABLE_THREADS
    audio_stop_thread(a);
#endif
    if (a->kind == AUDIO_WASAPI) {
        if (a->client && a->wasapi_started) IAudioClient_Stop(a->client);
        if (a->render) IAudioRenderClient_Release(a->render);
        if (a->client) IAudioClient_Release(a->client);
        if (a->device) IMMDevice_Release(a->device);
        if (a->mixfmt) CoTaskMemFree(a->mixfmt);
        if (a->com_initialized) CoUninitialize();
    }
    if (a->wave) {
        waveOutReset(a->wave);
        for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
            if (a->hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(a->wave, &a->hdr[i], sizeof(a->hdr[i]));
            free(a->wave_buf[i]);
        }
        waveOutClose(a->wave);
    } else {
        for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) free(a->wave_buf[i]);
    }
    free(a->queue);
    free(a->tmp);
#if GP32EMU_ENABLE_THREADS
    if (a->lock_ready) DeleteCriticalSection(&a->lock);
#endif
    free(a);
}


void gp32_win64_audio_reset(gp32_win64_audio_t *a) {
    if (!a) return;
#if GP32EMU_ENABLE_THREADS
    if (a->threaded_pump && a->pump_event) SetEvent(a->pump_event);
#endif
    audio_lock(a);
    a->read_frame = 0;
    a->frame_count = 0;
    a->underrun = 0;
    a->playback_started = 0;
    a->last_ring_l = 0;
    a->last_ring_r = 0;
    a->have_last_ring = 0;
    gp32_audio_resampler_reset(&a->resampler);
    audio_unlock(a);
    if (a->kind == AUDIO_WAVEOUT && a->wave) {
        waveOutReset(a->wave);
        for (unsigned i = 0; i < GP32_AUDIO_BUFFERS; ++i) {
            if (a->hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(a->wave, &a->hdr[i], sizeof(a->hdr[i]));
            memset(&a->hdr[i], 0, sizeof(a->hdr[i]));
        }
    } else if (a->kind == AUDIO_WASAPI && a->client) {
        if (a->wasapi_started) IAudioClient_Stop(a->client);
        IAudioClient_Reset(a->client);
        a->wasapi_started = 0;
    }
}

uint32_t gp32_win64_audio_buffered_frames(const gp32_win64_audio_t *a) {
    if (!a) return 0u;
#if GP32EMU_ENABLE_THREADS
    gp32_win64_audio_t *w = (gp32_win64_audio_t *)a;
    audio_lock(w);
    uint32_t n = a->frame_count;
    audio_unlock(w);
    return n;
#else
    return a->frame_count;
#endif
}
const char *gp32_win64_audio_backend(const gp32_win64_audio_t *a) { return a && a->backend_name[0] ? a->backend_name : "none"; }
const char *gp32_win64_audio_error(const gp32_win64_audio_t *a) { return a && a->error[0] ? a->error : ""; }
