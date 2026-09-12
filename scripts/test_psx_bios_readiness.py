#!/usr/bin/env python3
"""Run production PSX readiness and embedded-SDK tests with synthetic files.

Requires GCC or Clang with C++17. Optional roots must contain outputs from a
completed owned-input setup. This test does not install or launch a game.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--baseline-ref', help='Test the selected historical build.cpp; expected failures prove the regression')
    parser.add_argument('--owned-root', action='append', default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='retcomm bios test ') as work:
        work = Path(work)
        exe = work / ('check.exe' if os.name == 'nt' else 'check')
        source = root/'src/build/build.cpp'
        if args.baseline_ref:
            source=work/'baseline_build.cpp'
            source.write_bytes(subprocess.check_output(['git','-C',str(root),'show',args.baseline_ref+':src/build/build.cpp']))
        driver=work/'driver.cpp'
        driver.write_text((root/'scripts/fixtures/psx_bios_readiness.cpp').read_text().replace('#include "../../src/build/build.cpp"', '#include "'+source.as_posix()+'"'))
        cmd = [args.cxx, '-std=c++17', '-O2', '-flto', '-ffunction-sections', '-fdata-sections',
               '-I'+str(root/'include'), '-I'+str(root/'third_party'),
               str(driver),
               str(root/'src/paths/paths.cpp'),
               '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
               '-o', str(exe)]
        if os.name == 'nt': cmd += ['-lshell32']
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe), str(work/'fixtures'), *args.owned_root], check=True)

if __name__ == '__main__': main()
