/*
 * cellVdec, for real.
 *
 * ps3recomp registers cellVdecOpen/DecodeAu/GetPicture and friends, but the
 * title never calls them: it uses the *Ex* entry points, which nothing
 * registers. So cellVdecQueryAttrEx returned an unresolved-NID error, the game
 * read a memory requirement of zero ("Allocating 0 k for Vdec"), never opened
 * the decoder, and cellVdecStartSeq then failed with CELL_VDEC_ERROR_ARG.
 *
 * The four missing NIDs came out of the import table: the stubs are laid out
 * per module and sorted by NID, so the unregistered entries sitting inside the
 * cellVdec/cellAdec runs are the Ex variants.
 *
 *     0xC982A84A cellVdecQueryAttrEx    0x0053E2D8 cellVdecOpenEx
 *     0x7E4A4A49 cellAdecQueryAttrEx    0x8B5551A4 cellAdecOpenEx
 *
 * ps3recomp's cellVdec also declares cellVdecGetPicture with cellVdecGetPicItem's
 * signature, and its CellVdecPicItem is an invented layout rather than the PS3
 * one, so both are re-implemented here against the real ABI. Registering these
 * as ctx handlers overrides the generic table (ppu_hle.cpp checks g_ctx first).
 *
 * Decoding itself is in vdec_mf.cpp, on the Media Foundation H.264 MFT.
 */

#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" {
void     ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
uint32_t vm_read32(uint64_t a);
uint64_t vm_read64(uint64_t a);
void     vm_write8(uint64_t a, uint8_t v);
void     vm_write32(uint64_t a, uint32_t v);
void     vm_write64(uint64_t a, uint64_t v);
uint64_t ppu_guest_call(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7);
extern uint8_t* vm_base;

/* vdec_mf.cpp */
int  tm_vdec_open(int slot, int width, int height);
void tm_vdec_close(int slot);
int  tm_vdec_feed(int slot, const uint8_t* data, uint32_t size, int64_t pts);
int  tm_vdec_peek(int slot, const uint8_t** nv12, int* w, int* h, int64_t* pts);
void tm_vdec_pop(int slot);
int  tm_vdec_pending(int slot);
}

namespace {

/* --- error codes / enums, from the SDK ---------------------------------- */
constexpr int32_t  VDEC_ERROR_ARG   = (int32_t)0x80610101;
constexpr int32_t  VDEC_ERROR_SEQ   = (int32_t)0x80610102;
constexpr int32_t  VDEC_ERROR_EMPTY = (int32_t)0x80610104;

constexpr uint32_t MSG_AUDONE  = 0;
constexpr uint32_t MSG_PICOUT  = 1;
constexpr uint32_t MSG_SEQDONE = 2;

/* CellVdecPicFormat::formatType */
constexpr uint32_t PICFMT_ARGB32_ILV     = 0;
constexpr uint32_t PICFMT_RGBA32_ILV     = 1;
constexpr uint32_t PICFMT_UYVY422_ILV    = 2;
constexpr uint32_t PICFMT_YUV420_PLANAR  = 3;

/* CellVdecPicItem, PS3 layout. Offsets matter -- the title reads these. */
constexpr uint32_t PI_codecType = 0x00;
constexpr uint32_t PI_startAddr = 0x04;
constexpr uint32_t PI_size      = 0x08;
constexpr uint32_t PI_auNum     = 0x0C;   /* u8 */
constexpr uint32_t PI_auPts     = 0x10;   /* CellCodecTimeStamp[2] = {u32 upper, u32 lower} */
constexpr uint32_t PI_auDts     = 0x20;
constexpr uint32_t PI_userData  = 0x30;   /* u64[2] */
constexpr uint32_t PI_status    = 0x40;   /* s32 */
constexpr uint32_t PI_attr      = 0x44;   /* u8 */
constexpr uint32_t PI_picInfo   = 0x48;   /* pointer */
constexpr uint32_t PI_SIZE      = 0x4C;

constexpr int MAX_SLOTS = 4;

/* Handles must not be zero. The title stores the vdec handle at +0xC0 of its
 * video-stream object and treats 0 as "no decoder": func_0079A634 returns 155
 * without ever calling cellVdecDecodeAu, so the AVI streamed, the demuxer ran
 * and the video decoded nothing. Slot 0 is a perfectly legal index and a
 * useless handle -- hand out a nonzero id and map back. */
constexpr uint32_t VDEC_HANDLE_BASE = 0x0DEC0000u;
constexpr uint32_t ADEC_HANDLE_BASE = 0x0ADE0000u;
inline int vdec_index(uint32_t h) { return (h - VDEC_HANDLE_BASE) < (uint32_t)MAX_SLOTS
                                          ? (int)(h - VDEC_HANDLE_BASE) : -1; }
inline int adec_index(uint32_t h) { return (h - ADEC_HANDLE_BASE) < (uint32_t)MAX_SLOTS
                                          ? (int)(h - ADEC_HANDLE_BASE) : -1; }

struct Slot {
    int      in_use;
    int      seq;
    uint32_t codec;
    uint32_t cb_opd;      /* guest OPD of the message callback */
    uint32_t cb_arg;
    uint32_t scratch;     /* guest memory for the pic item + avc info */
    uint32_t au_num;
    int      w, h;
    /* the AU currently reported by GetPicItem */
    uint32_t pic_start, pic_size;
    uint64_t pic_pts, pic_dts, pic_user;
    int      have_pic;
};

Slot g_slot[MAX_SLOTS];

/* Playback stops after a couple of seconds; these say which side stops. */
long long g_n_decode = 0, g_n_picout = 0, g_n_getitem = 0, g_n_getpic = 0, g_n_empty = 0;
void vdec_stats(const char* why)
{
    static clock_t last = 0;
    const clock_t now = clock();
    if (last && (now - last) < 5 * CLOCKS_PER_SEC) return;
    last = now;
    fprintf(stderr, "[vdec] %s: DecodeAu=%lld PICOUT=%lld GetPicItem=%lld(empty %lld)"
                    " GetPicture=%lld pending=%d\n",
            why, g_n_decode, g_n_picout, g_n_getitem, g_n_empty, g_n_getpic,
            tm_vdec_pending(0));
    fflush(stderr);
}

int dbg()
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("TM_VDEC_DBG"); v = e ? atoi(e) : 0; }
    return v;
}

void callback(Slot* s, uint32_t handle, uint32_t msg, int32_t data)
{
    if (!s->cb_opd) return;
    ppu_guest_call(s->cb_opd, handle, msg, (uint64_t)(int64_t)data, s->cb_arg, 0, 0, 0, 0);
}

/* NV12 -> whatever the title asked for, written straight into guest memory. */
void write_picture(uint32_t out, const uint8_t* nv12, int w, int h, uint32_t fmt, uint8_t alpha)
{
    const uint8_t* y  = nv12;
    const uint8_t* uv = nv12 + (size_t)w * h;
    uint8_t* dst = vm_base + out;

    if (fmt == PICFMT_YUV420_PLANAR) {
        memcpy(dst, y, (size_t)w * h);
        uint8_t* du = dst + (size_t)w * h;
        uint8_t* dv = du + (size_t)(w / 2) * (h / 2);
        for (int j = 0; j < h / 2; j++)
            for (int i = 0; i < w / 2; i++) {
                du[(size_t)j * (w / 2) + i] = uv[(size_t)j * w + i * 2 + 0];
                dv[(size_t)j * (w / 2) + i] = uv[(size_t)j * w + i * 2 + 1];
            }
        return;
    }

    if (fmt == PICFMT_UYVY422_ILV) {
        for (int j = 0; j < h; j++) {
            const uint8_t* yr = y + (size_t)j * w;
            const uint8_t* cr = uv + (size_t)(j / 2) * w;
            uint8_t* d = dst + (size_t)j * w * 2;
            for (int i = 0; i < w; i += 2) {
                *d++ = cr[i];        /* U */
                *d++ = yr[i];
                *d++ = cr[i + 1];    /* V */
                *d++ = yr[i + 1];
            }
        }
        return;
    }

    /* ARGB / RGBA, BT.601 full-range-ish. The guest is big-endian, so a pixel
     * written as A,R,G,B bytes reads back as 0xAARRGGBB there. */
    for (int j = 0; j < h; j++) {
        const uint8_t* yr = y + (size_t)j * w;
        const uint8_t* cr = uv + (size_t)(j / 2) * w;
        uint8_t* d = dst + (size_t)j * w * 4;
        for (int i = 0; i < w; i++) {
            const int Y = (int)yr[i] - 16;
            const int U = (int)cr[(i & ~1) + 0] - 128;
            const int V = (int)cr[(i & ~1) + 1] - 128;
            int r = (298 * Y + 409 * V + 128) >> 8;
            int g = (298 * Y - 100 * U - 208 * V + 128) >> 8;
            int b = (298 * Y + 516 * U + 128) >> 8;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            if (fmt == PICFMT_RGBA32_ILV) {
                *d++ = (uint8_t)r; *d++ = (uint8_t)g; *d++ = (uint8_t)b; *d++ = alpha;
            } else {
                *d++ = alpha; *d++ = (uint8_t)r; *d++ = (uint8_t)g; *d++ = (uint8_t)b;
            }
        }
    }
}

/* --- entry points ------------------------------------------------------- */

/* cellVdecQueryAttrEx(const CellVdecTypeEx*, CellVdecAttr*) */
void cellVdecQueryAttrEx(ppu_context* ctx)
{
    const uint32_t type = (uint32_t)ctx->gpr[3];
    const uint32_t attr = (uint32_t)ctx->gpr[4];
    if (!attr) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }

    /* memSize is what the title allocates and hands back in OpenEx. The real
     * decoder wants a few MB; nothing here uses it except as pic-item scratch,
     * but it must be plausible or the title's own bookkeeping rejects it. */
    vm_write32(attr + 0x00, 4u * 1024 * 1024);   /* memSize   */
    vm_write8 (attr + 0x04, 4);                  /* cmdDepth  */
    vm_write8 (attr + 0x05, 0);
    vm_write8 (attr + 0x06, 0);
    vm_write8 (attr + 0x07, 0);
    if (dbg())
        fprintf(stderr, "[vdec] QueryAttrEx(type=0x%08X codec=%u) -> memSize=4M\n",
                type, type ? vm_read32(type) : 0);
    ctx->gpr[3] = 0;
}

/* cellVdecOpenEx(const CellVdecTypeEx*, const CellVdecResourceEx*,
 *                const CellVdecCb*, u32* handle) */
void cellVdecOpenEx(ppu_context* ctx)
{
    const uint32_t type   = (uint32_t)ctx->gpr[3];
    const uint32_t res    = (uint32_t)ctx->gpr[4];
    const uint32_t cb     = (uint32_t)ctx->gpr[5];
    const uint32_t handle = (uint32_t)ctx->gpr[6];
    if (!type || !handle) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }

    int idx = -1;
    for (int i = 0; i < MAX_SLOTS; i++) if (!g_slot[i].in_use) { idx = i; break; }
    if (idx < 0) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }

    Slot* s = &g_slot[idx];
    memset(s, 0, sizeof(*s));
    s->in_use = 1;
    s->codec  = vm_read32(type + 0x00);
    if (cb) { s->cb_opd = vm_read32(cb + 0x00); s->cb_arg = vm_read32(cb + 0x04); }

    /* Park the pic item at the top of the block the title allocated for us. */
    const uint32_t mem_addr = res ? vm_read32(res + 0x00) : 0;
    const uint32_t mem_size = res ? vm_read32(res + 0x04) : 0;
    s->scratch = (mem_addr && mem_size > 0x200) ? (mem_addr + mem_size - 0x200) : 0;

    s->w = 1280; s->h = 720;
    tm_vdec_open(idx, s->w, s->h);

    vm_write32(handle, VDEC_HANDLE_BASE + (uint32_t)idx);
    fprintf(stderr, "[vdec] OpenEx: codec=%u cb=0x%08X arg=0x%08X mem=0x%08X+%u -> handle %d\n",
            s->codec, s->cb_opd, s->cb_arg, mem_addr, mem_size, idx);
    ctx->gpr[3] = 0;
}

Slot* slot_of(ppu_context* ctx, uint32_t* out_h = nullptr, int* out_i = nullptr)
{
    const uint32_t h = (uint32_t)ctx->gpr[3];
    if (out_h) *out_h = h;
    const int i = vdec_index(h);
    if (out_i) *out_i = i;
    if (i < 0 || !g_slot[i].in_use) return nullptr;
    return &g_slot[i];
}

void cellVdecStartSeq(ppu_context* ctx)
{
    Slot* s = slot_of(ctx);
    if (!s) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    s->seq = 1;
    s->au_num = 0;
    fprintf(stderr, "[vdec] StartSeq\n");
    ctx->gpr[3] = 0;
}

void cellVdecEndSeq(ppu_context* ctx)
{
    uint32_t h = 0;
    Slot* s = slot_of(ctx, &h);
    if (!s) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    s->seq = 0;
    callback(s, h, MSG_SEQDONE, 0);
    ctx->gpr[3] = 0;
}

void cellVdecClose(ppu_context* ctx)
{
    uint32_t h = 0; int i = -1;
    Slot* s = slot_of(ctx, &h, &i);
    if (!s) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    tm_vdec_close(i);
    s->in_use = 0;
    ctx->gpr[3] = 0;
}

/* cellVdecDecodeAu(handle, mode, const CellVdecAuInfo*) */
void cellVdecDecodeAu(ppu_context* ctx)
{
    uint32_t h = 0; int i = -1;
    Slot* s = slot_of(ctx, &h, &i);
    const uint32_t au = (uint32_t)ctx->gpr[5];
    if (!s || !au) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    if (!s->seq)   { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_SEQ; return; }

    const uint32_t start = vm_read32(au + 0x00);
    const uint32_t size  = vm_read32(au + 0x04);
    const uint64_t pts   = vm_read64(au + 0x08);
    const uint64_t dts   = vm_read64(au + 0x10);
    const uint64_t user  = vm_read64(au + 0x18);

    s->au_num++;
    g_n_decode++;
    vdec_stats("decode");
    if (dbg() && s->au_num <= 8)
        fprintf(stderr, "[vdec] AU %u addr=0x%08X size=%u pts=%llu  %02X %02X %02X %02X %02X %02X\n",
                s->au_num, start, size, (unsigned long long)pts,
                vm_base[start + 0], vm_base[start + 1], vm_base[start + 2],
                vm_base[start + 3], vm_base[start + 4], vm_base[start + 5]);

    if (start && size)
        tm_vdec_feed(i, vm_base + start, size, (int64_t)pts);

    /* The AU buffer is the title's; tell it we are done with it before the
     * picture callback, which is the order the SDK documents. */
    callback(s, h, MSG_AUDONE, 0);

    if (tm_vdec_pending(i) > 0) {
        s->pic_start = start; s->pic_size = size;
        s->pic_pts = pts; s->pic_dts = dts; s->pic_user = user;
        s->have_pic = 1;
        g_n_picout++;
        callback(s, h, MSG_PICOUT, 0);
    }
    ctx->gpr[3] = 0;
}

/* cellVdecGetPicItem(handle, CellVdecPicItem** picItem) */
void cellVdecGetPicItem(ppu_context* ctx)
{
    uint32_t h = 0; int i = -1;
    Slot* s = slot_of(ctx, &h, &i);
    const uint32_t out = (uint32_t)ctx->gpr[4];
    if (!s || !out) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    g_n_getitem++;
    if (!tm_vdec_pending(i) || !s->scratch) {
        g_n_empty++;
        vdec_stats("item-empty");
        ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_EMPTY;
        return;
    }

    int w = 0, hgt = 0; int64_t pts = 0; const uint8_t* nv12 = nullptr;
    tm_vdec_peek(i, &nv12, &w, &hgt, &pts);

    const uint32_t pi = s->scratch;
    const uint32_t ai = s->scratch + PI_SIZE;      /* CellVdecAvcInfo after it */
    for (uint32_t o = 0; o < PI_SIZE; o += 4) vm_write32(pi + o, 0);

    vm_write32(pi + PI_codecType, s->codec);
    vm_write32(pi + PI_startAddr, s->pic_start);
    vm_write32(pi + PI_size,      s->pic_size);
    vm_write8 (pi + PI_auNum,     1);
    vm_write32(pi + PI_auPts + 0, (uint32_t)(s->pic_pts >> 32));
    vm_write32(pi + PI_auPts + 4, (uint32_t)s->pic_pts);
    vm_write32(pi + PI_auDts + 0, (uint32_t)(s->pic_dts >> 32));
    vm_write32(pi + PI_auDts + 4, (uint32_t)s->pic_dts);
    vm_write64(pi + PI_userData,  s->pic_user);
    vm_write32(pi + PI_status,    0);
    vm_write8 (pi + PI_attr,      0);
    vm_write32(pi + PI_picInfo,   ai);

    /* CellVdecAvcInfo: the title reads the picture size from here. */
    for (uint32_t o = 0; o < 0x80; o += 4) vm_write32(ai + o, 0);
    vm_write32(ai + 0x00, ((uint32_t)w << 16) | (uint32_t)hgt);  /* horizontal/vertical u16 pair */

    vm_write32(out, pi);
    if (dbg() > 1) fprintf(stderr, "[vdec] GetPicItem -> 0x%08X (%dx%d)\n", pi, w, hgt);
    ctx->gpr[3] = 0;
}

/* cellVdecGetPicture(handle, const CellVdecPicFormat*, u8* outBuff) */
void cellVdecGetPicture(ppu_context* ctx)
{
    uint32_t h = 0; int i = -1;
    Slot* s = slot_of(ctx, &h, &i);
    const uint32_t fmt_p = (uint32_t)ctx->gpr[4];
    const uint32_t out   = (uint32_t)ctx->gpr[5];
    if (!s) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }

    const uint8_t* nv12 = nullptr; int w = 0, hgt = 0; int64_t pts = 0;
    if (!tm_vdec_peek(i, &nv12, &w, &hgt, &pts) || !nv12) {
        ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_EMPTY;
        return;
    }

    const uint32_t fmt   = fmt_p ? vm_read32(fmt_p + 0x00) : PICFMT_ARGB32_ILV;
    const uint8_t  alpha = fmt_p ? (uint8_t)vm_read32(fmt_p + 0x08) : 0xFF;

    /* TM_VDEC_DUMP=<n>: write frame n as a BMP straight from the decoder, so
     * "is H.264 actually decoding" is answerable without involving the RSX. */
    { static int want = -1;
      if (want < 0) { const char* e = getenv("TM_VDEC_DUMP"); want = e ? atoi(e) : 0; }
      static long long fr = 0;
      if (want && ++fr == want) {
          const uint32_t stride = (uint32_t)(w * 3 + 3) & ~3u;
          uint8_t* img = (uint8_t*)calloc((size_t)stride * hgt, 1);
          if (img) {
              for (int j = 0; j < hgt; j++) {
                  uint8_t* row = img + (size_t)(hgt - 1 - j) * stride;
                  const uint8_t* yr = nv12 + (size_t)j * w;
                  const uint8_t* cr = nv12 + (size_t)w * hgt + (size_t)(j / 2) * w;
                  for (int i = 0; i < w; i++) {
                      const int Y = yr[i] - 16, U = cr[(i & ~1)] - 128, V = cr[(i & ~1) + 1] - 128;
                      int r = (298*Y + 409*V + 128) >> 8, g = (298*Y - 100*U - 208*V + 128) >> 8,
                          b = (298*Y + 516*U + 128) >> 8;
                      row[i*3+0] = (uint8_t)(b<0?0:(b>255?255:b));
                      row[i*3+1] = (uint8_t)(g<0?0:(g>255?255:g));
                      row[i*3+2] = (uint8_t)(r<0?0:(r>255?255:r));
                  }
              }
              FILE* f = fopen("vdec_frame.bmp", "wb");
              if (f) {
                  const uint32_t isz = stride * (uint32_t)hgt, fsz = 54 + isz, dof = 54, hsz = 40;
                  uint8_t hd[54] = {0}; hd[0]='B'; hd[1]='M';
                  memcpy(hd+2,&fsz,4); memcpy(hd+10,&dof,4); memcpy(hd+14,&hsz,4);
                  int32_t ww = w, hh = hgt; memcpy(hd+18,&ww,4); memcpy(hd+22,&hh,4);
                  hd[26]=1; hd[28]=24; memcpy(hd+34,&isz,4);
                  fwrite(hd,1,54,f); fwrite(img,1,isz,f); fclose(f);
                  fprintf(stderr, "[vdec] wrote vdec_frame.bmp (frame %d, %dx%d)\n", want, w, hgt);
              }
              free(img);
          }
      } }
    if (out) write_picture(out, nv12, w, hgt, fmt, alpha ? alpha : 0xFF);

    static long long n = 0;
    if ((n++ % 60) == 0)
        fprintf(stderr, "[vdec] GetPicture #%lld fmt=%u %dx%d -> 0x%08X\n", n, fmt, w, hgt, out);

    g_n_getpic++;
    vdec_stats("getpic");
    tm_vdec_pop(i);
    s->have_pic = 0;
    ctx->gpr[3] = 0;
}

/* --- cellAdec: enough to let the audio path open and stay quiet ---------- */

void cellAdecQueryAttrEx(ppu_context* ctx)
{
    const uint32_t attr = (uint32_t)ctx->gpr[4];
    if (!attr) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    vm_write32(attr + 0x00, 512u * 1024);
    vm_write32(attr + 0x04, 0);
    ctx->gpr[3] = 0;
}

/* The audio path only has to stop failing. cellSurMixerCreate is unavailable
 * here anyway, so nothing would be heard; but the movie player waits for every
 * stream to report ready, and an AC3 decoder that errors on the first AU makes
 * "MoviePlayer SyncLoaded timed out!" repeat forever and the video never
 * starts. Accept the AUs and acknowledge them. */
struct AdecSlot { int in_use, seq; uint32_t cb_opd, cb_arg;
                  uint32_t pcm_buf, pcm_bytes, item; };
AdecSlot g_adec[MAX_SLOTS];

constexpr uint32_t ADEC_MSG_AUDONE  = 0;
constexpr uint32_t ADEC_MSG_PCMOUT  = 1;
constexpr uint32_t ADEC_MSG_SEQDONE = 3;

void adec_cb(AdecSlot* a, uint32_t handle, uint32_t msg, int32_t data)
{
    if (a->cb_opd)
        ppu_guest_call(a->cb_opd, handle, msg, (uint64_t)(int64_t)data, a->cb_arg, 0, 0, 0, 0);
}

void cellAdecOpenEx(ppu_context* ctx)
{
    const uint32_t cb     = (uint32_t)ctx->gpr[5];
    const uint32_t handle = (uint32_t)ctx->gpr[6];
    /* The AVI carries three AC3 streams and the player opens one decoder per
     * stream, so handing every caller slot 0 made them share state. */
    int ai = -1;
    for (int k = 0; k < MAX_SLOTS; k++) if (!g_adec[k].in_use) { ai = k; break; }
    if (ai < 0) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    AdecSlot* a = &g_adec[ai];
    memset(a, 0, sizeof(*a));
    a->in_use = 1;
    if (cb) { a->cb_opd = vm_read32(cb + 0x00); a->cb_arg = vm_read32(cb + 0x04); }
    /* One AC3 frame is 1536 samples; hand back that much silence every time so
     * the audio stream completes its load handshake and stops holding up
     * MoviePlayer::SyncLoaded, which is what keeps the video from starting. */
    const uint32_t res = (uint32_t)ctx->gpr[4];
    const uint32_t mem = res ? vm_read32(res + 0x00) : 0;
    const uint32_t msz = res ? vm_read32(res + 0x04) : 0;
    if (mem && msz > 0x20000) {
        a->pcm_buf   = mem;
        a->pcm_bytes = 1536u * 6u * 4u;          /* 6ch float */
        a->item      = mem + msz - 0x100;
        for (uint32_t o = 0; o < a->pcm_bytes; o += 4) vm_write32(a->pcm_buf + o, 0);
    }
    if (handle) vm_write32(handle, ADEC_HANDLE_BASE + (uint32_t)ai);
    fprintf(stderr, "[adec] OpenEx: cb=0x%08X arg=0x%08X -> handle 0 (AUs acknowledged, no PCM)\n",
            a->cb_opd, a->cb_arg);
    ctx->gpr[3] = 0;
}

void cellAdecStartSeq(ppu_context* ctx)
{
    const int h = adec_index((uint32_t)ctx->gpr[3]);
    if (h < 0) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    g_adec[h].in_use = 1;
    g_adec[h].seq = 1;
    fprintf(stderr, "[adec] StartSeq\n");
    ctx->gpr[3] = 0;
}

void cellAdecEndSeq(ppu_context* ctx)
{
    const int h = adec_index((uint32_t)ctx->gpr[3]);
    if (h < 0) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    g_adec[h].seq = 0;
    adec_cb(&g_adec[h], ADEC_HANDLE_BASE + (uint32_t)h, ADEC_MSG_SEQDONE, 0);
    ctx->gpr[3] = 0;
}

void cellAdecDecodeAu(ppu_context* ctx)
{
    const int h = adec_index((uint32_t)ctx->gpr[3]);
    if (h < 0 || !g_adec[h].in_use) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    static long long n = 0;
    if (n++ < 4) fprintf(stderr, "[adec] DecodeAu #%lld\n", n);
    adec_cb(&g_adec[h], ADEC_HANDLE_BASE + (uint32_t)h, ADEC_MSG_AUDONE, 0);
    if (g_adec[h].pcm_buf) adec_cb(&g_adec[h], ADEC_HANDLE_BASE + (uint32_t)h, ADEC_MSG_PCMOUT, 0);
    ctx->gpr[3] = 0;
}

/* cellAdecGetPcm(handle, float* outBuffer) -- silence. */
void cellAdecGetPcm(ppu_context* ctx)
{
    const uint32_t h = (uint32_t)ctx->gpr[3];
    const uint32_t out = (uint32_t)ctx->gpr[4];
    if (h >= MAX_SLOTS || !g_adec[h].in_use) { ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_ARG; return; }
    if (out) for (uint32_t o = 0; o < g_adec[h].pcm_bytes; o += 4) vm_write32(out + o, 0);
    static long long n = 0;
    if (n++ < 3) fprintf(stderr, "[adec] GetPcm -> %u bytes of silence\n", g_adec[h].pcm_bytes);
    ctx->gpr[3] = 0;
}

/* cellAdecGetPcmItem(handle, CellAdecPcmItem** item) */
void cellAdecGetPcmItem(ppu_context* ctx)
{
    const int h = adec_index((uint32_t)ctx->gpr[3]);
    const uint32_t out = (uint32_t)ctx->gpr[4];
    AdecSlot* a = (h >= 0) ? &g_adec[h] : nullptr;
    if (!a || !a->in_use || !out || !a->item) {
        ctx->gpr[3] = (uint64_t)(int64_t)VDEC_ERROR_EMPTY; return;
    }
    for (uint32_t o = 0; o < 0x40; o += 4) vm_write32(a->item + o, 0);
    vm_write32(a->item + 0x00, ADEC_HANDLE_BASE + (uint32_t)h);  /* pcmHandle */
    vm_write32(a->item + 0x04, 0);              /* status = OK */
    vm_write32(a->item + 0x08, a->pcm_buf);     /* startAddr */
    vm_write32(a->item + 0x0C, a->pcm_bytes);   /* size */
    vm_write32(out, a->item);
    static long long n = 0;
    if (n++ < 3) fprintf(stderr, "[adec] GetPcmItem -> 0x%08X\n", a->item);
    ctx->gpr[3] = 0;
}

void cellAdecClose(ppu_context* ctx)       { const int h=adec_index((uint32_t)ctx->gpr[3]);
                                             if (h >= 0) g_adec[h].in_use = 0;
                                             ctx->gpr[3] = 0; }


/* The cellFs import block has six entries nothing registers, and a movie
 * streamer reads through cellFsAio* -- which would explain why the AVI is
 * opened and then never read. Probe them before implementing: log the NID and
 * arguments so the mapping is measured rather than guessed. */
void fs_probe(ppu_context* ctx, uint32_t nid)
{
    static long long n = 0;
    if (n++ < 24)
        fprintf(stderr, "[fsprobe] nid=0x%08X r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X lr=0x%08X\n",
                nid, (uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
                (uint32_t)ctx->gpr[6], (uint32_t)ctx->lr);
    ctx->gpr[3] = 0;
}
void fs_p0(ppu_context* c) { fs_probe(c, 0x2796FDF3u); }
void fs_p1(ppu_context* c) { fs_probe(c, 0x8CB722D5u); }
void fs_p2(ppu_context* c) { fs_probe(c, 0x9B882495u); }
void fs_p3(ppu_context* c) { fs_probe(c, 0x9F951810u); }
void fs_p4(ppu_context* c) { fs_probe(c, 0xCB588DBAu); }
void fs_p5(ppu_context* c) { fs_probe(c, 0xDB869F20u); }

}  // namespace

extern "C" void tm_vdec_register(void)
{
    if (getenv("TM_FSPROBE")) {
        ps3_hle_register_ctx(0x2796FDF3u, "fs?0", fs_p0);
        ps3_hle_register_ctx(0x8CB722D5u, "fs?1", fs_p1);
        ps3_hle_register_ctx(0x9B882495u, "fs?2", fs_p2);
        ps3_hle_register_ctx(0x9F951810u, "fs?3", fs_p3);
        ps3_hle_register_ctx(0xCB588DBAu, "fs?4", fs_p4);
        ps3_hle_register_ctx(0xDB869F20u, "fs?5", fs_p5);
    }
    /* The Ex variants nothing else registers -- this is what was missing. */
    ps3_hle_register_ctx(0xC982A84Au, "cellVdecQueryAttrEx", cellVdecQueryAttrEx);
    ps3_hle_register_ctx(0x0053E2D8u, "cellVdecOpenEx",      cellVdecOpenEx);
    ps3_hle_register_ctx(0x7E4A4A49u, "cellAdecQueryAttrEx", cellAdecQueryAttrEx);
    ps3_hle_register_ctx(0x8B5551A4u, "cellAdecOpenEx",      cellAdecOpenEx);
    ps3_hle_register_ctx(0x487B613Eu, "cellAdecStartSeq",    cellAdecStartSeq);
    ps3_hle_register_ctx(0xE2EA549Bu, "cellAdecEndSeq",      cellAdecEndSeq);
    ps3_hle_register_ctx(0x1529E506u, "cellAdecDecodeAu",    cellAdecDecodeAu);
    ps3_hle_register_ctx(0x97FF2AF1u, "cellAdecGetPcm",      cellAdecGetPcm);
    ps3_hle_register_ctx(0xBD75F78Bu, "cellAdecGetPcmItem",  cellAdecGetPcmItem);
    ps3_hle_register_ctx(0x847D2380u, "cellAdecClose",       cellAdecClose);

    /* Override ps3recomp's stubs: its GetPicture has GetPicItem's signature and
     * its CellVdecPicItem is not the PS3 layout. */
    ps3_hle_register_ctx(0xC757C2AAu, "cellVdecStartSeq",  cellVdecStartSeq);
    ps3_hle_register_ctx(0x824433F0u, "cellVdecEndSeq",    cellVdecEndSeq);
    ps3_hle_register_ctx(0x2BF4DDD2u, "cellVdecDecodeAu",  cellVdecDecodeAu);
    ps3_hle_register_ctx(0x17C702B9u, "cellVdecGetPicItem", cellVdecGetPicItem);
    ps3_hle_register_ctx(0x807C861Au, "cellVdecGetPicture", cellVdecGetPicture);
    ps3_hle_register_ctx(0x16698E83u, "cellVdecClose",     cellVdecClose);
}
