#!/usr/bin/env python3
"""Rewrite ACNH's three modules and verify them (d2r's structck + equiv); record the result for prepare_runtime.py.

The report (analysis/rewrite-validation.json, local only) holds each rewritten image's SHA-256 and both verdicts. A
module passes when the structural check is OK and the equivalence check found no divergent site. equiv.py also
reports FAIL when its mutation self-test misses a mutation that cannot change behaviour (a prefetch hint replaced by
nop): those misses are listed, and any other miss fails the module.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
D2R = ROOT.parent / 'totk/third_party/d2r'
PY = sys.executable
LLVM = os.environ.get('LLVM_BIN', '/opt/homebrew/opt/llvm@21/bin')
MODULES = ['main', 'subsdk0', 'sdk']


def run(args, env=None):
    p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, env={**os.environ, **(env or {})})
    return p.returncode, p.stdout + p.stderr


def main():
    out = ROOT / 'rewritten'
    out.mkdir(exist_ok=True)
    (ROOT / 'analysis').mkdir(exist_ok=True)
    report = {'status': 'pass', 'modules': {}}
    for name in MODULES:
        nso = ROOT / 'extracted/exefs' / name
        if not (out / f'{name}.patched.bin').exists() or '--rewrite' in sys.argv:
            code, text = run([PY, str(D2R / 'patch_nso.py'), str(nso), str(out), name],
                             {'DM_RULESET': 'native', 'DM_PRIVATE_GATES': '0'})
            if code: sys.exit(f'{name}: rewrite failed\n{text}')
        code, struct_text = run([PY, str(D2R / 'structck.py'), str(out), name, str(nso)], {'LLVM_BIN': LLVM})
        struct_line = struct_text.strip().splitlines()[-1]
        code, equiv_text = run([PY, str(D2R / 'equiv.py'), str(out), name, str(nso), '--jobs', str(os.cpu_count())])
        equiv_line = equiv_text.strip().splitlines()[-1]
        misses = [l for l in equiv_text.splitlines() if l.startswith('MUTMISS')]
        harmless = [l for l in misses if re.search(r'\bnop prfm\b', l)]
        divergent = int(re.search(r'分歧站点=(\d+)', equiv_line).group(1))
        ok = struct_line.startswith(f'STRUCT {name} OK') and divergent == 0 and len(misses) == len(harmless)
        digest = hashlib.sha256((out / f'{name}.patched.bin').read_bytes()).hexdigest()
        report['modules'][name] = {'sha256': digest, 'struct': struct_line, 'equiv': equiv_line,
                                   'harmless_mutation_misses': harmless, 'pass': ok}
        print(f'{name}: {"pass" if ok else "FAIL"} ({struct_line.split()[2]}, {divergent} divergent, '
              f'{len(harmless)} harmless mutation misses)')
        if not ok: report['status'] = 'fail'
    (ROOT / 'analysis/rewrite-validation.json').write_text(json.dumps(report, indent=2) + '\n')
    sys.exit(0 if report['status'] == 'pass' else 1)


if __name__ == '__main__':
    main()
