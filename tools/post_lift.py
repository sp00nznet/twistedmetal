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
"""
import glob
import re
import sys

# guest address -> reason. The lifted definition is renamed to <name>_lifted;
# src/hle_extra.cpp defines the replacement under the original name.
OVERRIDES = {
    '0034ACAC': "the title's log(level, fmt, ...)",
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
}

RECOMP = 'src/recomp'


def patch(path, changed):
    src = open(path, encoding='utf-8', errors='surrogateescape').read()
    out = src
    for addr in list(OVERRIDES) + list(TRACE):
        definition = f'void func_{addr}(ppu_context* ctx) {{'
        if definition in out:
            out = out.replace(definition, f'void func_{addr}_lifted(ppu_context* ctx) {{')
            changed.append(f'{path}: renamed func_{addr} definition')
    if out != src:
        open(path, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(out)
        return True
    return False


def main():
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
