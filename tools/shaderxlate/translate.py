"""Translate one NVN stage program (control + data sections) to SPIR-V plus a manifest.

    python translate.py <corpus_dir> <key> [--out dir] [--dump] [--mode structured|dispatcher]
"""
import argparse
import os
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import lift      # noqa: E402
import emit      # noqa: E402
from container import NvnStage   # noqa: E402

TRANSLATOR = 'shaderxlate 1'
SPIRV_VAL = os.environ.get('SPIRV_VAL', r'A:\AI\Recomps\new-pokemon-snap\tools\vulkan_sdk\Bin\spirv-val.exe')


class Failed(Exception):
    pass


def program_of(stage, control, data):
    st = NvnStage(stage, control, data)
    prog = lift.Program(stage, st.code, st.sph, st.consts, st.key)
    if stage == 'cs':
        prog.local_size = local_size(control)
    return prog


def nvn_stage(control):
    """Stage named by the control section (word 0x714: NVN shader stage 0 vertex, 1 fragment,
    2 geometry, 3 tess control, 4 tess evaluation, 5 compute)."""
    v = struct.unpack_from('<I', control, 0x714)[0]
    return {0: 'vs', 1: 'fs', 2: 'gs', 3: 'tcs', 4: 'tes', 5: 'cs'}.get(v)


def local_size(control):
    """Compute workgroup size from the control section (words 0x748, 0x74c, 0x750)."""
    ls = struct.unpack_from('<3I', control, 0x748)
    if not all(1 <= x <= 1024 for x in ls) or ls[0] * ls[1] * ls[2] > 1024:
        raise Failed('compute local size %r out of range' % (ls,))
    return ls


def translate(stage, control, data, mode='structured'):
    """Returns (spv_bytes, manifest). Raises Failed(reason)."""
    prog = program_of(stage, control, data)
    try:
        fn, info = lift.lift(prog)
        spv, em = emit.emit(fn, info, prog, mode)
    except (lift.Unsupported, emit.EmitError) as e:
        raise Failed(str(e))
    man = em.manifest(prog.key)
    man['insns'] = info.insns
    man['translator'] = TRANSLATOR
    return spv, man


def validate(spv_path):
    """spirv-val for Vulkan 1.3 semantics. Returns (ok, message)."""
    r = subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.3', spv_path], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('corpus')
    ap.add_argument('key')
    ap.add_argument('--stage')
    ap.add_argument('--out', default='.')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--mode', default='structured')
    a = ap.parse_args()
    data = open(os.path.join(a.corpus, a.key + '.data'), 'rb').read()
    ctrl = open(os.path.join(a.corpus, a.key + '.ctrl'), 'rb').read()
    stage = a.stage or nvn_stage(ctrl) or 'fs'
    if a.dump:
        fn, info = lift.lift(program_of(stage, ctrl, data))
        print(fn.dump())
    spv, man = translate(stage, ctrl, data, a.mode)
    out = os.path.join(a.out, a.key + '.spv')
    open(out, 'wb').write(spv)
    ok, msg = validate(out)
    print(man.get('cf_mode'), 'spirv-val', 'OK' if ok else 'FAIL', msg[:2000])


if __name__ == '__main__':
    main()
