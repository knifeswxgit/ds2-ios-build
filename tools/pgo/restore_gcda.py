#!/usr/bin/env python3
"""v108: puts the .gcda files written by a DS2_PGO=GEN build (sdmc:/switch/ds2/pgo, object path with '/' -> '#')
back next to the objects of the build folder, for the DS2_PGO=USE build of the same folder.

usage: restore_gcda.py <folder with the files from the console> <build folder>
"""
import os
import shutil
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, build = sys.argv[1], os.path.abspath(sys.argv[2])
    done = skipped = 0
    for name in sorted(os.listdir(src)):
        if not name.endswith('.gcda'):
            continue
        path = name.replace('#', '/')
        if not os.path.isabs(path):
            path = os.path.join(build, path)
        if not os.path.isdir(os.path.dirname(path)):
            skipped += 1
            continue
        shutil.copyfile(os.path.join(src, name), path)
        done += 1
    print(f'{done} profiles restored, {skipped} without an object folder')


if __name__ == '__main__':
    main()
