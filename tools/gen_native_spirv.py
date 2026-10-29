"""Regenerate the native renderer's Vulkan SPIR-V header from the HLSL.

The D3D12 backend compiles src/native/shaders/*.hlsl at runtime; the Vulkan
backend runs the SPIR-V embedded in
src/native/shaders/spirv/skate3_native_shaders_spirv.h, compiled offline with
a SPIR-V-capable DXC (the Windows SDK's dxc.exe is built without SPIR-V
codegen; use the GitHub release, e.g. REXGLUE/_ext/dxc/bin/x64/dxc.exe).

Usage:
  gen_native_spirv.py --src SRC_DIR --dxc DXC_EXE [--out HEADER]
  gen_native_spirv.py --src SRC_DIR --check
      verify the layout specs below against every binding decorated in the
      existing header (no DXC needed).

Compile matrix: every MakeShaderDesc(stage, "file.hlsl", src, "entry", macros,
"variant") call with literal arguments in the renderer sources, plus the
(file, entry, variant) rows of the existing header (entries picked at
runtime, e.g. from name tables, and macro variants).

Bindings: the Vulkan RHI's set/binding plan (rexglue native_rhi_vulkan.cpp):
set 0 = the buffer params (constants / CBV / buffer SRV) in param order, then
the static samplers; sets 1..N = one per texture-table param in param order,
binding = index within the table. Each shader file is compiled against the
binding layout its pipelines use (LAYOUTS mirrors the C++ BindingLayoutDescs;
update it together with them). A register a shader declares that its layout
does not cover is an error.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

# (kind, register, count) in param order; kinds: buf (constants, CBV or
# buffer SRV: one set-0 binding), table (texture table: its own set).
SCENE = {  # g_r.layout, EnsureRootSignature (skate3_native_scene_gpu.cpp)
    'params': [('buf', 'b0', 1), ('table', 't0', 1), ('table', 't1', 1), ('buf', 't2', 1),
               ('table', 't3', 1), ('table', 't4', 2), ('buf', 'b1', 1), ('table', 't6', 2),
               ('table', 't8', 5), ('buf', 'b2', 1)],
    'samplers': 2,
}
AO = {  # g_r.ao_layout (skate3_native_scene_post.cpp): SSAO / HBAO
    'params': [('buf', 'b0', 1), ('table', 't0', 1), ('table', 't1', 1)],
    'samplers': 2,
}
SSR = {  # g_r.ssr_layout (skate3_native_scene_post.cpp)
    'params': [('buf', 'b0', 1), ('table', 't0', 1), ('table', 't1', 1), ('table', 't2', 1)],
    'samplers': 2,
}
HDR = {  # g_r.hdr_layout (skate3_native_scene_post.cpp)
    'params': [('buf', 'b0', 1), ('table', 't0', 1), ('table', 't1', 1), ('table', 't2', 1),
               ('table', 't3', 1), ('table', 't4', 1), ('buf', 'b1', 1)],
    'samplers': 2,
}
PFX = {  # g_r.pfx_layout (skate3_native_scene_gpu.cpp): photo-mode postfx
    'params': [('buf', 'b0', 1), ('table', 't0', 8)],
    'samplers': 3,
}
LAYOUTS = {
    'scene.hlsl': SCENE, 'blur.hlsl': SCENE, 'outline.hlsl': SCENE, 'overlay2d.hlsl': SCENE,
    'spline.hlsl': SCENE, 'shadow_blur.hlsl': SCENE, 'resolve.hlsl': SCENE,
    'ssao.hlsl': AO, 'hbao.hlsl': AO, 'ssr.hlsl': SSR, 'hdr.hlsl': HDR,
    'photo_fx.hlsl': PFX,
}
RENDERER_SOURCES = ['skate3_native_scene_gpu.cpp', 'skate3_native_scene_post.cpp', 'skate3_vr.cpp']
# Macro variants of entries the renderer picks at runtime (name tables), which
# neither the literal-call scan nor a plain entry-name match can see.
EXTRA_VARIANTS = [
    ('hbao.hlsl', 'ps_hbao', 'HDR=1'),
]
HEADER_REL = os.path.join('native', 'shaders', 'spirv', 'skate3_native_shaders_spirv.h')
SHADER_REL = os.path.join('native', 'shaders')


def binding_map(layout):
    """register ('b0', 't5', 's1') -> (set, binding)."""
    out = {}
    set0 = 0
    table_set = 1
    for kind, reg, count in layout['params']:
        cls, base = reg[0], int(reg[1:])
        if kind == 'buf':
            out[reg] = (0, set0)
            set0 += 1
        else:
            for i in range(count):
                out[f'{cls}{base + i}'] = (table_set, i)
            table_set += 1
    for i in range(layout['samplers']):
        out[f's{i}'] = (0, set0 + i)
    return out


def declared_registers(shader_dir, fname, seen=None):
    """resource name -> register, over the file and its includes."""
    seen = seen if seen is not None else set()
    if fname in seen:
        return {}
    seen.add(fname)
    src = open(os.path.join(shader_dir, fname), encoding='utf-8').read()
    src = re.sub(r'//[^\n]*', '', src)
    regs = {}
    for inc in re.findall(r'#include\s+"([^"]+)"', src):
        regs.update(declared_registers(shader_dir, inc, seen))
    for m in re.finditer(r'(\w+)\s*(?:\[\d*\])?\s*:\s*register\(\s*([btsu])(\d+)', src):
        regs[m.group(1)] = f'{m.group(2)}{m.group(3)}'
    return regs


def existing_rows(header_text):
    return re.findall(r'\{"([^"]+)", "([^"]+)", "([^"]*)", (k_\w+), sizeof', header_text)


def existing_arrays(header_text):
    arrays = {}
    for m in re.finditer(r'static const uint32_t (k_\w+)\[\] = \{(.*?)\};', header_text, re.S):
        arrays[m.group(1)] = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', m.group(2))]
    return arrays


def spirv_bindings(words):
    """resource variable name -> (set, binding) from a SPIR-V module."""
    names, sets, binds = {}, {}, {}
    i = 5
    while i < len(words):
        op, wc = words[i] & 0xFFFF, words[i] >> 16
        if wc == 0:
            break
        a = words[i + 1:i + wc]
        if op == 5:  # OpName
            raw = b''.join(w.to_bytes(4, 'little') for w in a[1:])
            names[a[0]] = raw.split(b'\0')[0].decode('utf-8', 'replace')
        elif op == 71 and a[1] == 34:  # OpDecorate DescriptorSet
            sets[a[0]] = a[2]
        elif op == 71 and a[1] == 33:  # OpDecorate Binding
            binds[a[0]] = a[2]
        i += wc
    return {names.get(v, f'%{v}'): (sets.get(v, 0), b) for v, b in binds.items()}


def matrix(src_dir, header_text):
    rows = set()
    call = re.compile(
        r'MakeShaderDesc\(\s*nrhi::ShaderStage::k(\w+),\s*"(\w+\.hlsl)",\s*\w+,\s*'
        r'(?:"(\w+)"|[^,]+),\s*[^,]+,\s*(?:"([^"]*)"|[^)]+)\)', re.S)
    for name in RENDERER_SOURCES:
        path = os.path.join(src_dir, name)
        if not os.path.exists(path):
            continue
        for m in call.finditer(open(path, encoding='utf-8').read()):
            if m.group(3) is not None and m.group(4) is not None:
                rows.add((m.group(2), m.group(3), m.group(4)))
    for f, e, v, _ in existing_rows(header_text):
        rows.add((f, e, v))
    # Entry functions picked through name tables: every vs_/ps_/cs_ function a
    # shader file defines whose name the renderer sources spell as a literal.
    literals = set()
    for name in RENDERER_SOURCES:
        path = os.path.join(src_dir, name)
        if os.path.exists(path):
            literals.update(re.findall(r'"((?:vs|ps|cs)_\w+)"', open(path, encoding='utf-8').read()))
    shader_dir = os.path.join(src_dir, SHADER_REL)
    for f in LAYOUTS:
        src = open(os.path.join(shader_dir, f), encoding='utf-8').read()
        for e in set(re.findall(r'^\s*[\w<>]+\s+((?:vs|ps|cs)_\w+)\s*\(', src, re.M)):
            if e in literals and not any(r[0] == f and r[1] == e for r in rows):
                rows.add((f, e, ''))  # macro-variant-only entries keep their variants
    rows.update(EXTRA_VARIANTS)
    order = list(LAYOUTS)
    return sorted(rows, key=lambda r: (order.index(r[0]) if r[0] in order else 99, r[1], r[2]))


def check(src_dir, header_text):
    shader_dir = os.path.join(src_dir, SHADER_REL)
    arrays = existing_arrays(header_text)
    bad = 0
    for f, e, v, arr in existing_rows(header_text):
        want = binding_map(LAYOUTS[f])
        regs = declared_registers(shader_dir, f)
        for var, sb in spirv_bindings(arrays[arr]).items():
            reg = regs.get(var) or regs.get(var.replace('type.', ''))
            if reg is None:
                print(f'  ? {f}:{e}[{v}] {var}={sb}: register not found in current HLSL')
                continue
            if want.get(reg) != sb:
                print(f'  MISMATCH {f}:{e}[{v}] {var} ({reg}): header {sb}, spec {want.get(reg)}')
                bad += 1
    for f in LAYOUTS:
        want = binding_map(LAYOUTS[f])
        missing = {n: r for n, r in declared_registers(shader_dir, f).items() if r not in want}
        if missing:
            print(f'  UNCOVERED {f}: {missing}')
            bad += 1
    print('check:', 'OK' if bad == 0 else f'{bad} problem(s)')
    return bad == 0


def sanitize(variant):
    return re.sub(r'[^A-Za-z0-9]+', '_', variant).strip('_')


def compile_all(src_dir, dxc, out_path, header_text):
    shader_dir = os.path.join(src_dir, SHADER_REL)
    rows = matrix(src_dir, header_text)
    version = subprocess.run([dxc, '--version'], capture_output=True, text=True).stdout.strip()
    blobs = []
    with tempfile.TemporaryDirectory() as tmp:
        for f, entry, variant in rows:
            layout = binding_map(LAYOUTS[f])
            regs = set(declared_registers(shader_dir, f).values())
            uncovered = sorted(r for r in regs if r not in layout)
            if uncovered:
                sys.exit(f'{f}: registers {uncovered} not covered by its binding layout '
                         f'(update LAYOUTS in {os.path.basename(__file__)})')
            profile = {'vs': 'vs_6_0', 'ps': 'ps_6_0', 'cs': 'cs_6_0'}[entry.split('_')[0]]
            out = os.path.join(tmp, 'out.spv')
            args = [dxc, '-spirv', '-fspv-target-env=vulkan1.1', '-O3', '-T', profile, '-E',
                    entry, '-I', shader_dir, '-Fo', out]
            for macro in filter(None, variant.split(';')):
                args += ['-D', macro]
            for reg in sorted(regs):
                st, bd = layout[reg]
                args += ['-fvk-bind-register', reg, '0', str(bd), str(st)]
            args.append(os.path.join(shader_dir, f))
            r = subprocess.run(args, capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f'dxc failed for {f}:{entry} [{variant}]:\n{r.stderr}')
            data = open(out, 'rb').read()
            words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
            blobs.append((f, entry, variant, words))
            print(f'  {f}:{entry} [{variant}] {len(data)} bytes')
    lines = [
        '// Generated file -- DO NOT EDIT. Compiled offline with DXC from the HLSL',
        '// sources in src/native/shaders by tools/gen_native_spirv.py (rerun it, or',
        '// let the build do so, whenever one of them changes).',
        f'// Compiler: {version.splitlines()[0] if version else "dxc"}',
        '// Flags: -spirv -fspv-target-env=vulkan1.1 -O3, register bindings per',
        '// the frozen Vulkan set/binding plan the RHI backend expects.',
        '#pragma once', '', '#include <cstddef>', '#include <cstdint>', '',
        'namespace skate3::native_spirv {', '']
    names = []
    for f, entry, variant, words in blobs:
        name = f'k_{f.split(".")[0]}_{entry}' + (f'_{sanitize(variant)}' if variant else '')
        names.append((f, entry, variant, name))
        tag = f' [{variant}]' if variant else ''
        lines.append(f'// {f} : {entry}{tag} ({len(words) * 4} bytes)')
        lines.append(f'static const uint32_t {name}[] = {{')
        for i in range(0, len(words), 8):
            lines.append('    ' + ', '.join(f'0x{w:08x}' for w in words[i:i + 8]) + ',')
        lines.append('};')
        lines.append('')
    lines += [
        'struct NativeSpirvBlob {',
        '  const char* file;      // e.g. "scene.hlsl"',
        '  const char* entry;     // e.g. "vs_main"',
        '  const char* variant;   // canonical macro signature, e.g. "SAMPLES=4";',
        '                         // "" for no macros',
        '  const uint32_t* data;',
        '  size_t size_bytes;',
        '};', '',
        'static const NativeSpirvBlob kNativeSpirvBlobs[] = {']
    for f, entry, variant, name in names:
        lines.append(f'    {{"{f}", "{entry}", "{variant}", {name}, sizeof({name})}},')
    lines += ['};', '', 'static const size_t kNativeSpirvBlobCount =',
              '    sizeof(kNativeSpirvBlobs) / sizeof(kNativeSpirvBlobs[0]);', '',
              '}  // namespace skate3::native_spirv', '']
    tmp_out = out_path + '.tmp'
    with open(tmp_out, 'w', encoding='utf-8', newline='\n') as fo:
        fo.write('\n'.join(lines))
    os.replace(tmp_out, out_path)
    print(f'wrote {len(blobs)} blobs to {out_path}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', required=True, help='the game src/ directory')
    ap.add_argument('--dxc', help='SPIR-V-capable dxc.exe')
    ap.add_argument('--out', help='header to write (default: the checked-in one)')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    header = os.path.join(a.src, HEADER_REL)
    header_text = open(header, encoding='utf-8').read() if os.path.exists(header) else ''
    if a.check:
        sys.exit(0 if check(a.src, header_text) else 1)
    if not a.dxc:
        sys.exit('--dxc is required to compile')
    compile_all(a.src, a.dxc, a.out or header, header_text)


if __name__ == '__main__':
    main()
