#!/usr/bin/env python
# -*- encoding: utf8 -*-

# ----------------------------------------------------------------------------
# Copyright (C) 2012 Jolla Ltd.
# Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
# License: GPLv2
# ----------------------------------------------------------------------------

import sys,os

def is_local(path):
    return not path.startswith("/")

def print_rule(dest, srce):
    print "%s\\\n" % "\\\n\t".join(["%s:" % dest] + srce)

def set_extension(path, ext):
    return os.path.splitext(path)[0] + ext

def normalize_path(srce):
    return os.path.normpath(srce)

def fix_directory(dest, srce):
    srce = os.path.dirname(srce)
    dest = os.path.basename(dest)
    return os.path.join(srce, dest)

if __name__ == "__main__":

    data = sys.stdin.readlines()
    data = map(lambda x:x.rstrip(), data)
    data.reverse()

    deps = []

    # Note: dependencies are sorted only to keep
    #       future diffs cleaner

    while data:
        line = data.pop()
        while line.endswith('\\'):
            line = line[:-1] + data.pop()
        if not ':' in line:
            continue

        dest,srce = line.split(":",1)
        srce = srce.split()
        srce,temp = srce[0],srce[1:]

        # take dest path primary srce
        dest = fix_directory(dest, srce)

        # remove secondary deps with absolute path
        temp = filter(is_local, temp)

        # sort secondary sources
        temp.sort()

        srce = [srce] + temp
        srce = map(normalize_path, srce)

        deps.append((dest,srce))

    for dest,srce in sorted(deps):
        # emit normal compilation dependencies
        print_rule(dest, srce)
        # and for -fPIC in case it is needed
        dest = set_extension(dest, ".pic.o")
        print_rule(dest, srce)
