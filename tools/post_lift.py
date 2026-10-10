#!/usr/bin/env python3
"""Idempotent post-lift patches for the generated C++ under src/recomp.

The lifter emits every guest function as a direct C++ call, so
ppu_register_function -- which only redirects INDIRECT dispatch -- cannot
replace one. Overriding a guest function therefore means renaming its
definition here and providing the replacement in src/hle_extra.cpp.

Currently one patch: the title's own logger at guest 0x0034ACAC,
log(level, fmt, ...). Its output goes nowhere in this port, which hides the
whole boot trace -- and in a stripped binary that kept 731 Class::method
strings, that trace is the most useful instrument we have. Renaming the lifted
body lets hle_extra.cpp implement it against the host stderr, gated on
TM_GAMELOG so a normal run stays quiet.

Run after every re-lift:  python tools/post_lift.py
A lift of another binary passes its title id:  --title BCES01010
"""
import argparse
import glob
import re
import sys

# guest address -> reason. The lifted definition is renamed to <name>_lifted;
# src/hle_extra.cpp defines the replacement under the original name.
OVERRIDES = {
    '0034ACAC': "the title's log(level, fmt, ...)",
    '00980B20': "the title's second log(level, fmt, ...) -- FIOS, ArchiveLoader, WorldLoader",
    '004740EC': "the title's third log(channel, fmt, ...) -- the Ui state machine",
    '009CF718': 'TEMP diag: BRB driver block callback (call counter)',
    '009D9590': 'TEMP diag: BRB output processing (call counter)',
    '009D4200': 'TEMP diag: BRB completion poll (call counter)',
    '009BBE60': 'BRB bank free-to-unload check: always free under TM_BRB_SILENT',
    '009D7B08': 'BRB driver thread loop: idles under TM_BRB_SILENT',
    '004DBEC4': 'TEMP: memalign caller trace (TM_ALLOCTRACE)',
    '004DBB70': 'main heap init: TM_HEAP_MB overrides the fixed 15 MB',
    '0062EB40': 'TEMP diag: world query (cycle probe)',
    '00514EF8': 'TEMP diag: AI line-of-sight raycast (NaN probe)',
    '000DB394': 'TEMP diag: per-frame players update (call counter)',
    '0010C898': 'TEMP diag: frame update (call counter)',
    '00676ED8': 'TEMP diag: particle-pool init (logs that it ran)',
    '00678CE8': 'TEMP diag: particle-pool drain wait (dumps the pool once)',
    '009BBFB8': "BRB list append: skip a node already in the list (TM_SKIP_BANKUNLOAD re-registers banks)",
}

# Guest functions to WRAP rather than replace: the lifted body is renamed the
# same way, and src/hle_extra.cpp defines a tracer under the original name that
# logs r3 in/out and calls through. Enabled at run time with TM_TRACE=1.
# These four are RTApp::initHardware's renderer init and its three main
# callees -- the init returns 0 and that is what ends the boot.
TRACE = {
    '00671698': 'renderer init (this, 1280, 720)',
    '00670C10': 'renderer init callee',
    '00671560': 'renderer init callee',
    '006A9430': 'renderer init callee',
    '0076C534': 'fios scheduler ctor (builds m_objectLock..m_workerLock)',
    '0076CDF4': 'fios createSchedulerForMedia',
    '0077A088': 'fios Mutex::lock',
    '00779C18': 'fios Mutex ctor (this, name) -> sys_lwmutex_create',
    '0076CB00': 'fios worker setup (writes the object that is never constructed)',
    '0076756C': 'candidate element ctor (copies a vtable to +0x00)',
    '007556B4': 'fios object base ctor (writes the "FIOS obj ...." tag)',
    '0099790C': 'edge decompressor wait(this, request, ...) -- dumps the request',
    '0020C26C': 'WorldLoader::loadGame',
    '0020C33C': 'WorldLoader::loadUi',
    '0020C478': 'WorldLoader::loadCinema',
    '0035FBFC': 'ArchiveLoader::waitIO',
    '0035FD84': 'ArchiveLoader::load',
    '003609AC': 'ArchiveLoader::thread',
    '00474110': 'UiLegal_1::onEnter',
    '00474198': 'UiLegal_1::update',
    '0047447C': 'UiLegal_2::onEnter',
    '00474710': 'UiLegal_3::onEnter',
    '00474A68': 'UiLegal_4::onEnter',
    '00475150': 'UiLegal_Health::onEnter',
    '00475450': 'UiLegal_ESRB::onEnter',
    '005BA908': 'UiState::enter',
    '005BB358': 'UiState::update',
    '005CAEA0': 'UiState::setTimer',
    '004AA780': 'UiNetShutdown::onEnter (NOT IntroMovie -- its own log string names it)',
    '004AAA98': 'UiNetShutdown::update',
    '006AD8E8': 'MoviePlayer::a',
    '006ADA38': 'MoviePlayer::b',
    '004B6270': 'Movie::c',
    '004B6580': 'Movie::d',
    '006AC648': 'MoviePlayer::openFile',
    '0036255C': 'ArchiveLoader::moviesPath',
    '000120C4': 'attractScript',
    '0020C7F8': 'WorldLoader::createWorld (reads script factory +0x3F8)',
    '003A82C8': 'world ctor (takes the script factory in r9)',
    '002B8F38': 'scriptFactory(the one loadGame is normally given)',
    '00059F6C': 'movieNameForId',
    '001DB604': 'playMovieFile',
    '00799B5C': 'movie video-stream path',
    '0079A634': 'movie video-stream path',
    '00799EF0': 'movie video-stream path',
    '0079A1D8': 'movie video-stream path',
    '009B2574': 'cinema load: untraced library callee',
    '009B2784': 'cinema load: untraced library callee',
    '00606F78': 'cinema world load chain',
    '00606CE4': 'cinema world load chain',
    '0036168C': 'cinema world load chain',
    '00361750': 'cinema world load chain',
    '005F8FF4': 'cinema world load chain',
    '00606B6C': 'cinema world load chain',
    '00607670': 'cinema world load chain',
    '0060E500': 'cinema world load chain',
    '00617730': 'cinema world load chain',
    '007675D0': 'cinema world load chain',
    '0076B66C': 'cinema world load chain',
    '004BDA30': 'cinema world init chain',
    '00681630': 'cinema world init chain',
    '005A0AA0': 'cinema world init chain',
    '005A9034': 'cinema world init chain',
    '005A92A0': 'cinema world init chain',
    '006A1FC8': 'cinema world init chain',
    '001111EC': 'cinema world init chain',
    '003C00FC': 'cinema world init chain',
    '006107E8': 'cinema world init chain',
    '005B1D64': 'cinema world init chain',
    '000CBC3C': 'cinema world init chain',
    '003499E0': 'cinema world init chain',
    '003A87C4': 'cinema setup chain',
    '003A9050': 'cinema setup chain',
    '003AA034': 'cinema setup chain',
    '003BE230': 'cinema setup chain',
    '003BE810': 'cinema setup chain',
    '004DDCE0': 'cinema setup chain',
    '004DDD04': 'cinema setup chain',
    '004DDFD0': 'cinema setup chain',
    '00667DF8': 'cinema setup chain',
    '00669F8C': 'cinema setup chain',
    '003C24B4': 'WorldLoader::setup callee',
    '003A8A20': 'WorldLoader::setup callee',
    '004DCB4C': 'WorldLoader::setup callee',
    '003A78C8': 'WorldLoader::setup callee',
    '003622B0': 'WorldLoader::setup callee',
    '0005A028': 'forms avi names st_mid/mg_mid/df_mid (table A)',
    '0005A82C': 'forms an avi name from table A',
    '003D40E0': 'forms st_intro/mg_intro/df_intro + mid/end (table C) -- the campaign cinematics',
    '003D7BE0': 'forms st_intro (table C)',
    '003F1AF0': 'forms mg_intro/df_intro (table C2)',
    '003F33D0': 'forms mg_intro/df_intro (table C2)',
    '0066DD48': 'references avi table A (0x00C7DD34)',
    '002997B8': 'references avi table B (0x00C897B4)',
    '0056EF3C': 'references avi table D (0x00CBBED0)',
    '0056F7B0': 'references avi table D',
    '00692924': 'references avi table D',
    '00693600': 'references avi table D',
    '00693AD0': 'references avi table D',
    '00081D88': 'references avi table D',
    '001DB6B8': 'vtable 0x00CE78E0 slot 1, references avi table B',
    '001DB4FC': 'ctor of the class whose vtable slot 0 is playMovieFile (vtable 0x00CE78E0)',
    '0014BCA8': 'boot::seq',
    '0020C698': 'WorldLoader::setup',
    '00360B90': 'AL::b90',
    '00360C3C': 'AL::c3c',
    '00360E84': 'AL::e84',
    '00360EBC': 'AL::ebc',
    '00360F68': 'AL::f68',
    '00360F88': 'AL::f88',
    '0064BA08': 'updateLoadBar::frame',
    '0064C410': 'loadBar::finish -- sets the load-complete byte',
    '0010E57C': 'setLoadDone -- the other writer of that byte',
    '0036204C': 'AL::204c',
    '004AA5D8': 'a fourth log(level, fmt, ...)',
    '0034F060': 'stringTable::get(table, id) -- names the intro movie',
}

# Guest fragments the lifter emitted without a fall-through edge, mapped to the
# guest address execution should continue at. find_functions ended
# func_0076C534 at 0x0076CAFC while the real function runs to 0x0076CDF4, so the
# ~190 instructions in between became disconnected fragments. Every one of them
# ends in a trampoline except 0x0076CB00, whose loop simply falls off the end of
# the emitted function -- so when the loop exits, the rest of the FIOS scheduler
# constructor never runs, its workers are never built and the caller resumes
# with clobbered callee-saved registers.
FALLTHROUGH = {
    '0076CB00': '0076CBB4',
}

# Per-title tables. Every address above is a BCUS98106 one.
# BCES01010 (Europe, v01.01): the title's three loggers, found through the
# format strings they are passed, with the same bodies as their US
# counterparts, and the Edge decompressor's wait, the binary's only caller of
# cellSpursEventFlagWait. No tracer or other by-address fix is ported yet; the
# vector-op lowerings below are generic and apply to every title.
TITLES = {
    'BCUS98106': dict(overrides=OVERRIDES, trace=TRACE, fallthrough=FALLTHROUGH,
                      address_patches=True),
    'BCES01010': dict(overrides={
                          '0034AE6C': "the title's log(level, fmt, ...) (BCUS98106 0x0034ACAC)",
                          '00981860': "the title's second log (BCUS98106 0x00980B20)",
                          '004747BC': "the Ui state machine's log (BCUS98106 0x004740EC)",
                          '0099864C': "the Edge decompressor's completion wait (BCUS98106 0x0099790C)",
                          '00976FD4': "the title's dlmalloc free(mspace, mem), checked under TM_FREECHECK",
                      },
                      trace={}, fallthrough={}, address_patches=False),
}
# False for a title the TOI/segment probes below have not been ported to.
ADDRESS_PATCHES = True

# vsumsws sites this lift emitted as "/* TODO */" no-ops -- the lifter only
# learned the instruction afterwards (ps3recomp 93f1377). 1100 of them sit in
# Havok, mostly "count the lanes that passed": vcmpgefp, vand, vsumsws. With
# the sum missing, Havok reads a lane value as a count. Same lowering as the
# lifter: vr[] is big-endian bytes, saturated sum of vA's four words plus
# vB word 3 lands in word 3, the rest are zero.
VSUMSWS = re.compile(r'/\* TODO: vsumsws v(\d+), v(\d+), v(\d+) \*/;')


def vsumsws(m):
    vd, va, vb = m.groups()
    return ('{ const uint8_t* a=(const uint8_t*)&ctx->vr[%s]; '
            'const uint8_t* b=(const uint8_t*)&ctx->vr[%s]; uint8_t o[16]; '
            'memset(o,0,16); int64_t acc=0; '
            'for(int j=0;j<4;j++){ acc+=(int64_t)(int32_t)(((uint32_t)a[j*4]<<24)|'
            '((uint32_t)a[j*4+1]<<16)|((uint32_t)a[j*4+2]<<8)|a[j*4+3]); } '
            'acc+=(int64_t)(int32_t)(((uint32_t)b[12]<<24)|((uint32_t)b[13]<<16)|'
            '((uint32_t)b[14]<<8)|b[15]); '
            'if(acc>2147483647LL) acc=2147483647LL; '
            'if(acc<-2147483648LL) acc=-2147483648LL; '
            'uint32_t r=(uint32_t)(int32_t)acc; '
            'for(int k=0;k<4;k++) o[12+k]=(uint8_t)(r>>(8*(3-k))); '
            'memcpy(&ctx->vr[%s], o, 16); }' % (va, vb, vd))

# The other vector ops this lift left as "/* TODO */". Halfword lanes are
# big-endian byte pairs; word lanes go through the lifter's ppu_vldu4/vstu4.
VMXOPS = re.compile(r'/\* TODO: (vslh|vsrh|vsubuws|vpkuwum|vrfin|vexptefp) v(\d+), v(\d+), v(\d+) \*/;')
HALF = ('{ const uint8_t* a=(const uint8_t*)&ctx->vr[%(a)s]; const uint8_t* b=(const uint8_t*)&ctx->vr[%(b)s]; '
        'uint8_t o[16]; for(int i=0;i<8;i++){ uint16_t x=(uint16_t)((a[2*i]<<8)|a[2*i+1]); '
        'unsigned n=b[2*i+1]&15u; uint16_t r=(uint16_t)(%(expr)s); o[2*i]=(uint8_t)(r>>8); o[2*i+1]=(uint8_t)r; } '
        'memcpy(&ctx->vr[%(d)s], o, 16); }')


def vmxop(m):
    mn, d, a, b = m.groups()
    if mn == 'vslh':
        return HALF % dict(a=a, b=b, d=d, expr='x<<n')
    if mn == 'vsrh':
        return HALF % dict(a=a, b=b, d=d, expr='x>>n')
    if mn == 'vsubuws':
        return ('{ uint32_t x[4],y[4]; ppu_vldu4(&ctx->vr[%s],x); ppu_vldu4(&ctx->vr[%s],y); '
                'for(int i=0;i<4;i++) x[i]=x[i]>y[i]?x[i]-y[i]:0u; ppu_vstu4(&ctx->vr[%s],x); }' % (a, b, d))
    if mn == 'vpkuwum':
        return ('{ const uint8_t* x=(const uint8_t*)&ctx->vr[%s]; const uint8_t* y=(const uint8_t*)&ctx->vr[%s]; '
                'uint8_t o[16]; for(int i=0;i<4;i++){ o[2*i]=x[4*i+2]; o[2*i+1]=x[4*i+3]; '
                'o[8+2*i]=y[4*i+2]; o[8+2*i+1]=y[4*i+3]; } memcpy(&ctx->vr[%s], o, 16); }' % (a, b, d))
    # vrfin / vexptefp vD,vB: the disassembler prints them as vD, v0, vB.
    fn = 'nearbyintf' if mn == 'vrfin' else 'exp2f'
    return ('{ float x[4]; ppu_vldf4(&ctx->vr[%s],x); for(int i=0;i<4;i++) x[i]=%s(x[i]); '
            'ppu_vstf4(&ctx->vr[%s],x); }' % (b, fn, d))

# Havok's time-of-impact loop at guest 0x00B955D0 runs `while (t + dt < tEnd)`
# via fcmpu + cror(GT|EQ). An unordered compare (any NaN) never sets the exit
# bit, so a NaN that real hardware does not produce spins it forever on the
# match's first physics step. Exit when unordered, and report the operands.
# ponytail: guard, not a fix -- the NaN's producer is the real bug.
TOI_HEAD = ('ctx->fpr[0] = ppu_fp_single(ppu_fadd(ctx->fpr[25], ctx->fpr[18]));')  # the t + dt compare; the guard goes 3 lines on
TOI_GUARD = ('        if (ctx->cr & 1u) { extern void tm_toi_nan(ppu_context*); tm_toi_nan(ctx); goto loc_00B96088; }' + chr(10))

# TEMP diag: the segment-raycast loop in func_00630150 (back-edge to
# loc_00630288) steps t by `step` until t >= tmax; report its operands.
SEG_EDGE = '        goto loc_00630288;'
SEG_PROBE = '        { extern void tm_seg_probe(ppu_context*); tm_seg_probe(ctx); }'

RECOMP = 'src/recomp'


def patch(path, changed):
    src = open(path, encoding='utf-8', errors='surrogateescape').read()
    out = src
    for addr in list(OVERRIDES) + list(TRACE):
        definition = f'void func_{addr}(ppu_context* ctx) {{'
        if definition in out:
            out = out.replace(definition, f'void func_{addr}_lifted(ppu_context* ctx) {{')
            changed.append(f'{path}: renamed func_{addr} definition')
    for addr, nxt in FALLTHROUGH.items():
        head = f'void func_{addr}_lifted(ppu_context* ctx) {{'
        i = out.find(head)
        if i < 0:
            continue
        end = out.find('\n}\n', i)
        edge = f'        {{ g_trampoline_fn = (void(*)(void*))func_{nxt}; return; }}'
        if end > 0 and edge not in out[i:end]:
            out = out[:end] + '\n' + edge + out[end:]
            changed.append(f'{path}: func_{addr} falls through to func_{nxt}')

    out, n = VSUMSWS.subn(vsumsws, out)
    out, k = VMXOPS.subn(vmxop, out)
    if ADDRESS_PATCHES and SEG_EDGE in out and 'tm_seg_probe' not in out and 'func_00630150_' not in out and 'void func_00630150(' in out:
        eol = chr(13) + chr(10) if chr(13) + chr(10) in out else chr(10)
        out = out.replace(SEG_EDGE, SEG_PROBE + eol + SEG_EDGE)
        changed.append(f'{path}: probed segment raycast loop')
    if ADDRESS_PATCHES and TOI_HEAD in out and 'tm_toi_nan' not in out:
        eol = chr(13) + chr(10) if chr(13) + chr(10) in out else chr(10)
        exit_br = '        if (((ctx->cr >> 0) & 2)) goto loc_00B96088;'
        # TOI_HEAD, the fcmpu, the cror, then the exit branch: guard only there.
        out, g = re.subn(re.escape(TOI_HEAD) + r'((?:[^\n]*\n){3})' + re.escape(exit_br),
                         lambda m: TOI_HEAD + m.group(1) + TOI_GUARD.replace(chr(10), eol) + exit_br, out)
        changed.append(f'{path}: guarded {g} TOI loop heads')
    if k:
        changed.append(f'{path}: lowered {k} vector ops')
    if n:
        changed.append(f'{path}: lowered {n} vsumsws')
    if out != src:
        open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(out)
        return True
    return False


def main():
    global OVERRIDES, TRACE, FALLTHROUGH, ADDRESS_PATCHES
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--title', default='BCUS98106', choices=sorted(TITLES),
                    help='title id of the lifted EBOOT (default: BCUS98106)')
    args = ap.parse_args()
    t = TITLES[args.title]
    OVERRIDES, TRACE, FALLTHROUGH = t['overrides'], t['trace'], t['fallthrough']
    ADDRESS_PATCHES = t['address_patches']

    files = sorted(glob.glob(f'{RECOMP}/*.cpp'))
    if not files:
        print(f'no lifted sources under {RECOMP}/ -- run ppu_lifter first', file=sys.stderr)
        return 1

    changed = []
    for f in files:
        patch(f, changed)

    # The header still declares the original name, which is what hle_extra.cpp
    # defines, so it needs no edit -- but the renamed body needs a declaration.
    hdr = f'{RECOMP}/ppu_recomp.h'
    h = open(hdr, encoding='utf-8', errors='surrogateescape').read()
    for addr in list(OVERRIDES) + list(TRACE):
        decl = f'void func_{addr}_lifted(ppu_context* ctx);'
        if decl not in h:
            h = h.replace(f'void func_{addr}(ppu_context* ctx);',
                          f'void func_{addr}(ppu_context* ctx);\n{decl}')
            changed.append(f'{hdr}: declared func_{addr}_lifted')
    open(hdr, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(h)

    for c in changed:
        print(c)
    print(f'post_lift: {len(changed)} change(s)' if changed else 'post_lift: already applied')
    return 0


if __name__ == '__main__':
    sys.exit(main())
