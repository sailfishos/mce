#!/usr/bin/env python

# ----------------------------------------------------------------------------
# Copyright (C) 2012 Jolla Ltd.
# Contact: Simo Piiroinen <simo.piiroinen@jollamobile.com>
# License: LGPLv2
# ----------------------------------------------------------------------------

# reads mce gconf schemas and writes C structure array
# compatible with builtin-gconf.c

import sys,os

from types import *

from xml.etree.ElementTree import ElementTree

typemap = {
'bool' : 'b',
'int'  : 'i',
}

if __name__ == "__main__":
    args = sys.argv[1:]
    args.reverse()

    src = []
    _ = src.append

    elems = []

    while args:
        path = args.pop()
        tree = ElementTree()
        root = tree.parse(path)
        data = root.find("schemalist")

        for elem in data.iter("schema"):
            _("")

            e_key  = elem.find("applyto").text
            e_type = elem.find("type").text
            e_def  = elem.find("default").text
            e_arr  = ''

            if e_type == "list":
                e_arr = 'a'
                e_type = elem.find("list_type").text
                e_def = e_def.replace("[","").replace("]","")
                e_def = map(lambda x:x.strip(), e_def.split(","))
                e_def = ",".join(e_def)

            e_type = e_arr + typemap.get(e_type, '?')

            elems.append((e_key, e_type, e_def))

    _("")
    _('static setting_t elems[] =')
    _("{")

    for e_key, e_type, e_def in elems:
        _('  {')
        _('    .key  = "%s",' % e_key)
        _('    .type = "%s",' % e_type)
        _('    .def  = "%s",' % e_def)
        _('  },')

    _('};')

    print "\n".join(src)
