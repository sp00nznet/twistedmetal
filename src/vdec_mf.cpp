/*
 * H.264 decoding for cellVdec, on the Media Foundation decoder MFT.
 *
 * ps3recomp's cellVdec accepts access units and decodes nothing, so the title
 * opens its intro cinematic, demuxes it and shows a black screen. Decoding it
 * needs an H.264 implementation; Windows ships one (the "Microsoft H264 Video
 * Decoder MFT", a system component since Windows 7), so this uses that rather
 * than pulling in FFmpeg. No new dependency: mfplat/mfuuid/ole32 are part of
 * the OS.
 *
 * The MFT is fed one access unit at a time and produces NV12 frames. It buffers
 * internally -- the first few ProcessInput calls yield nothing while it collects
 * a GOP, and it renegotiates its output type once it has parsed the SPS, which
 * is the MF_E_TRANSFORM_STREAM_CHANGE path below.
 *
 * Frames come out into a small ring so a decode can run ahead of the guest's
 * GetPicture without stalling the demuxer.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mfobjects.h>
#include <initguid.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

/* CLSID_CMSH264DecoderMFT -- the stock Microsoft H.264 decoder. */
DEFINE_GUID(TM_CLSID_CMSH264DecoderMFT,
            0x62ce7e72, 0x4c71, 0x4d20, 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d);

namespace {

constexpr int MAX_SLOTS = 4;
constexpr int RING      = 8;

struct Frame {
    uint8_t* data;      /* NV12: Y plane then interleaved UV */
    uint32_t cap;
    int      w, h;
    int64_t  pts;       /* the guest's own pts, carried through */
    int      valid;
};

struct Slot {
    int             open;
    IMFTransform*   mft;
    int             w, h;
    int             out_w, out_h;   /* what the MFT actually produces */
    Frame           ring[RING];
    int             head, tail;     /* head = next write, tail = next read */
    long long       fed, produced;
};

Slot g_slot[MAX_SLOTS];
bool g_mf_started = false;

/* The MFT reports its stride in the output type; NV12 rows can be padded. */
int frame_bytes(int w, int h) { return w * h * 3 / 2; }

bool push_frame(Slot* s, const uint8_t* src, int stride, int w, int h, int64_t pts)
{
    const int next = (s->head + 1) % RING;
    if (next == s->tail) {
        /* Ring full: drop the oldest rather than block the decoder. */
        s->tail = (s->tail + 1) % RING;
    }
    Frame* f = &s->ring[s->head];
    const uint32_t need = (uint32_t)frame_bytes(w, h);
    if (f->cap < need) {
        free(f->data);
        f->data = (uint8_t*)malloc(need);
        f->cap = f->data ? need : 0;
        if (!f->data) return false;
    }
    /* Copy out of the MFT buffer, un-padding the stride. */
    uint8_t* d = f->data;
    for (int y = 0; y < h; y++) { memcpy(d, src + (size_t)y * stride, (size_t)w); d += w; }
    const uint8_t* uv = src + (size_t)stride * h;
    for (int y = 0; y < h / 2; y++) { memcpy(d, uv + (size_t)y * stride, (size_t)w); d += w; }
    f->w = w; f->h = h; f->pts = pts; f->valid = 1;
    s->head = next;
    return true;
}

IMFSample* make_sample(const uint8_t* data, uint32_t size, int64_t pts_100ns)
{
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buf = nullptr;
    if (FAILED(MFCreateSample(&sample))) return nullptr;
    if (FAILED(MFCreateMemoryBuffer(size, &buf))) { sample->Release(); return nullptr; }
    BYTE* p = nullptr; DWORD maxlen = 0, cur = 0;
    if (SUCCEEDED(buf->Lock(&p, &maxlen, &cur))) {
        memcpy(p, data, size);
        buf->Unlock();
        buf->SetCurrentLength(size);
        sample->AddBuffer(buf);
        sample->SetSampleTime(pts_100ns);
    }
    buf->Release();
    return sample;
}

/* (Re)negotiate the output type. Called at open and again whenever the MFT
 * reports a stream change, which it does once it has seen the SPS and knows the
 * real picture size. */
bool set_output_nv12(Slot* s)
{
    for (DWORD i = 0; ; i++) {
        IMFMediaType* t = nullptr;
        HRESULT hr = s->mft->GetOutputAvailableType(0, i, &t);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) return false;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            hr = s->mft->SetOutputType(0, t, 0);
            if (SUCCEEDED(hr)) {
                UINT32 w = 0, h = 0;
                MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
                if (w && h) { s->out_w = (int)w; s->out_h = (int)h; }
                t->Release();
                return true;
            }
        }
        t->Release();
    }
}

/* Pull every frame the MFT is willing to give us right now. */
void drain_output(Slot* s, int64_t pts)
{
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER out{};
        DWORD status = 0;
        MFT_OUTPUT_STREAM_INFO si{};
        s->mft->GetOutputStreamInfo(0, &si);

        IMFSample* sample = nullptr;
        /* When the MFT does not allocate its own samples we must provide one. */
        const bool provides =
            (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                           MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        if (!provides) {
            if (FAILED(MFCreateSample(&sample))) return;
            IMFMediaBuffer* b = nullptr;
            if (FAILED(MFCreateAlignedMemoryBuffer(si.cbSize ? si.cbSize : (DWORD)frame_bytes(s->out_w, s->out_h),
                                                   MF_16_BYTE_ALIGNMENT, &b))) {
                sample->Release(); return;
            }
            sample->AddBuffer(b);
            b->Release();
            out.pSample = sample;
        }

        HRESULT hr = s->mft->ProcessOutput(0, 1, &out, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (out.pSample) out.pSample->Release();
            return;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (out.pSample) out.pSample->Release();
            if (!set_output_nv12(s)) return;
            continue;                       /* try again with the new type */
        }
        if (FAILED(hr) || !out.pSample) {
            if (out.pSample) out.pSample->Release();
            return;
        }

        IMFMediaBuffer* buf = nullptr;
        if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(&buf)) && buf) {
            IMF2DBuffer* b2d = nullptr;
            BYTE* scan = nullptr; LONG pitch = 0;
            if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2d))) && b2d) {
                if (SUCCEEDED(b2d->Lock2D(&scan, &pitch))) {
                    push_frame(s, scan, (int)pitch, s->out_w, s->out_h, pts);
                    b2d->Unlock2D();
                    s->produced++;
                }
                b2d->Release();
            } else {
                BYTE* p = nullptr; DWORD ml = 0, cl = 0;
                if (SUCCEEDED(buf->Lock(&p, &ml, &cl))) {
                    push_frame(s, p, s->out_w, s->out_w, s->out_h, pts);
                    buf->Unlock();
                    s->produced++;
                }
            }
            buf->Release();
        }
        out.pSample->Release();
        if (out.pEvents) out.pEvents->Release();
    }
}

}  // namespace

extern "C" int tm_vdec_open(int slot, int width, int height)
{
    if (slot < 0 || slot >= MAX_SLOTS) return 0;
    Slot* s = &g_slot[slot];
    if (s->open) return 1;

    if (!g_mf_started) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
            fprintf(stderr, "[vdec] MFStartup failed -- no H.264 decoder\n");
            return 0;
        }
        g_mf_started = true;
    }

    memset(s, 0, sizeof(*s));
    s->w = width  > 0 ? width  : 1280;
    s->h = height > 0 ? height : 720;
    s->out_w = s->w; s->out_h = s->h;

    HRESULT hr = CoCreateInstance(TM_CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&s->mft));
    if (FAILED(hr) || !s->mft) {
        fprintf(stderr, "[vdec] no H.264 decoder MFT (hr=0x%08lX)\n", (unsigned long)hr);
        return 0;
    }

    /* Low-latency: the title drives playback itself and a reordering delay just
     * shows up as a stall at the start of the movie. */
    IMFAttributes* attr = nullptr;
    if (SUCCEEDED(s->mft->GetAttributes(&attr)) && attr) {
        attr->SetUINT32(MF_LOW_LATENCY, 1);
        attr->Release();
    }

    IMFMediaType* in = nullptr;
    if (FAILED(MFCreateMediaType(&in))) { s->mft->Release(); s->mft = nullptr; return 0; }
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32)s->w, (UINT32)s->h);
    MFSetAttributeRatio(in, MF_MT_FRAME_RATE, 30000, 1001);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = s->mft->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) {
        fprintf(stderr, "[vdec] SetInputType failed (hr=0x%08lX)\n", (unsigned long)hr);
        s->mft->Release(); s->mft = nullptr; return 0;
    }

    set_output_nv12(s);
    s->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    s->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    s->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    s->open = 1;
    fprintf(stderr, "[vdec] H.264 MFT open, %dx%d\n", s->w, s->h);
    return 1;
}

extern "C" void tm_vdec_close(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    Slot* s = &g_slot[slot];
    if (!s->open) return;
    if (s->mft) {
        s->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        s->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        s->mft->Release();
    }
    for (int i = 0; i < RING; i++) free(s->ring[i].data);
    fprintf(stderr, "[vdec] close: fed %lld AUs, produced %lld frames\n", s->fed, s->produced);
    memset(s, 0, sizeof(*s));
}

/* Feed one access unit. Returns the number of frames now waiting. */
extern "C" int tm_vdec_feed(int slot, const uint8_t* data, uint32_t size, int64_t pts)
{
    if (slot < 0 || slot >= MAX_SLOTS) return 0;
    Slot* s = &g_slot[slot];
    if (!s->open || !s->mft || !data || !size) return 0;

    IMFSample* sample = make_sample(data, size, pts);
    if (!sample) return 0;
    HRESULT hr = s->mft->ProcessInput(0, sample, 0);
    sample->Release();
    if (hr == MF_E_NOTACCEPTING) {
        drain_output(s, pts);
        sample = make_sample(data, size, pts);
        if (sample) { s->mft->ProcessInput(0, sample, 0); sample->Release(); }
    }
    s->fed++;
    drain_output(s, pts);
    return (s->head - s->tail + RING) % RING;
}

/* Hand back the oldest decoded frame without removing it. */
extern "C" int tm_vdec_peek(int slot, const uint8_t** nv12, int* w, int* h, int64_t* pts)
{
    if (slot < 0 || slot >= MAX_SLOTS) return 0;
    Slot* s = &g_slot[slot];
    if (!s->open || s->head == s->tail) return 0;
    Frame* f = &s->ring[s->tail];
    if (nv12) *nv12 = f->data;
    if (w) *w = f->w;
    if (h) *h = f->h;
    if (pts) *pts = f->pts;
    return 1;
}

extern "C" void tm_vdec_pop(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return;
    Slot* s = &g_slot[slot];
    if (!s->open || s->head == s->tail) return;
    s->ring[s->tail].valid = 0;
    s->tail = (s->tail + 1) % RING;
}

extern "C" int tm_vdec_pending(int slot)
{
    if (slot < 0 || slot >= MAX_SLOTS) return 0;
    Slot* s = &g_slot[slot];
    return s->open ? (s->head - s->tail + RING) % RING : 0;
}

#else   /* !_WIN32 */

extern "C" int  tm_vdec_open(int, int, int) { return 0; }
extern "C" void tm_vdec_close(int) {}
extern "C" int  tm_vdec_feed(int, const uint8_t*, uint32_t, int64_t) { return 0; }
extern "C" int  tm_vdec_peek(int, const uint8_t**, int*, int*, int64_t*) { return 0; }
extern "C" void tm_vdec_pop(int) {}
extern "C" int  tm_vdec_pending(int) { return 0; }

#endif
