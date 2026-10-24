"""Check the actual embedded WASM, not just the requested compiler flags."""
import argparse
import base64
import json
import re
from pathlib import Path


def varuint(data, at):
    value = shift = 0
    while at < len(data) and shift < 35:
        byte = data[at]
        at += 1
        value |= (byte & 127) << shift
        if byte < 128:
            return value, at
        shift += 7
    raise ValueError('invalid WASM section length')


def custom_sections(data):
    if data[:8] != b'\0asm\x01\0\0\0':
        raise ValueError('invalid WASM module')
    sections = []
    at = 8
    while at < len(data):
        section = data[at]
        size, start = varuint(data, at + 1)
        end = start + size
        if end > len(data):
            raise ValueError('truncated WASM section')
        if section == 0:
            length, name_start = varuint(data, start)
            if name_start + length > end:
                raise ValueError('truncated custom-section name')
            sections.append(data[name_start:name_start + length].decode('utf-8'))
        at = end
    return sections


def check(module, mode):
    js = module.read_text(encoding='utf-8')
    embedded = re.findall(r'data:application/octet-stream;base64,([A-Za-z0-9+/=]+)', js)
    if not embedded:
        raise ValueError('missing embedded WASM')
    wasm = base64.b64decode(max(embedded, key=len), validate=True)
    sections = custom_sections(wasm)
    if mode == 'stripped':
        forbidden = [s for s in sections if s.startswith('.debug_') or s in
                     ('name', 'producers', 'sourceMappingURL', 'external_debug_info', 'linking')]
        if forbidden:
            raise ValueError('Release exposes internal/debug metadata: ' + ', '.join(forbidden))
        if re.search(r'(?m)^\s*//[#@]\s*sourceMappingURL=', js):
            raise ValueError('Release glue references a source map')
    else:
        missing = {'name', '.debug_info', '.debug_line'} - set(sections)
        if missing:
            raise ValueError('debug configuration lost metadata: ' + ', '.join(sorted(missing)))
    # These public entry points are required by the existing JS wrapper in both modes.
    for export in ('_vgs_open', '_vgs_close', '_vgs_set_time', '_vgs_frame', '_vgs_error'):
        if export not in js:
            raise ValueError('public API missing: ' + export)
    return {'mode': mode, 'js_bytes': module.stat().st_size,
            'wasm_bytes': len(wasm), 'custom_sections': sections}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', type=Path)
    parser.add_argument('mode', choices=('stripped', 'debug'))
    args = parser.parse_args()
    print(json.dumps(check(args.module, args.mode), indent=2))
