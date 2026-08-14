#!/usr/bin/env python3
#
# Copyright (C) 2026 GB# contributors
#
# This software may be modified and distributed under the terms
# of the MIT license.  See the LICENSE file for details.
#
# Asserts that a built GB# runtime library is what it claims to be: no SDL, no
# OpenGL, no window system of any kind linked into it.
#
# The boundary is mostly enforced by the build already. A shared library has to
# resolve every symbol it uses on Windows and macOS, and cmake/gbsharp.cmake
# passes --no-undefined to make that true on Linux too, so adding host.c to the
# core sources fails to link rather than silently producing a library that
# needs SDL at load time.
#
# What that does not catch is a dependency that resolves because somebody also
# added the link libraries. This does: a linked library appears in the import
# table by name, and the import table is plain ASCII in the file. Scanning the
# bytes is cruder than parsing PE, ELF and Mach-O separately, but it needs no
# tooling that varies per runner, and there is no legitimate reason for the
# string "SDL" to be in this library at all.
import argparse
import os
import sys

FORBIDDEN = [
    (b'SDL', 'SDL2'),
    (b'libGL', 'OpenGL'),
    (b'OPENGL32', 'OpenGL'),
    (b'opengl32', 'OpenGL'),
    (b'imgui', 'imgui'),
]

# Every function the ABI promises. Checked as a set so that a build which
# quietly stops exporting one fails here rather than in a P/Invoke at runtime.
EXPECTED_EXPORTS = [
    'gbsharp_abi_version',
    'gbsharp_has_debug_support',
    'gbsharp_create',
    'gbsharp_destroy',
    'gbsharp_load_rom',
    'gbsharp_reset',
    'gbsharp_run_frame',
    'gbsharp_get_framebuffer',
    'gbsharp_get_audio',
    'gbsharp_set_button',
    'gbsharp_read_memory',
    'gbsharp_write_memory',
    'gbsharp_save_ram_size',
    'gbsharp_read_save_ram',
    'gbsharp_write_save_ram',
]


def check(path):
    with open(path, 'rb') as f:
        data = f.read()

    problems = []

    for needle, name in FORBIDDEN:
        if needle in data:
            problems.append('%s is linked into %s (found %r)' % (
                name, os.path.basename(path), needle))

    for symbol in EXPECTED_EXPORTS:
        if symbol.encode('ascii') not in data:
            problems.append('%s does not export %s' % (
                os.path.basename(path), symbol))

    return problems


def main(args):
    parser = argparse.ArgumentParser()
    parser.add_argument('libraries', metavar='library', nargs='+',
                        help='built shared libraries to check')
    options = parser.parse_args(args)

    problems = []
    for library in options.libraries:
        if not os.path.exists(library):
            problems.append('%s does not exist' % library)
            continue
        problems.extend(check(library))
        print('checked %s' % library)

    for problem in problems:
        print('ERROR: %s' % problem, file=sys.stderr)

    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
