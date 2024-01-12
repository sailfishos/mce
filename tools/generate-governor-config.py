#!/usr/bin/env python

import sys,os,glob

# Governor selection lists: Preferred ones first

PREFER_PERFORMANCE = ("performance",
                      "interactive",
                      "ondemand",
                      "conservative",
                      "userspace",
                      "powersave",
                     )

PREFER_INTERACTIVE = ("interactive",
                      "ondemand",
                      "conservative",
                      "performance",
                      "userspace",
                      "powersave",
                     )

PREFER_POWERSAVE = ("powersave",
                    "conservative",
                    "interactive",
                    "ondemand",
                    "performance",
                    "userspace",
                   )

# Config blocks to create
# 1: frequency selection "percentage"
# 2: config block name
# 3: governor selection list

CONFIG_BLOCKS = ((80, "Performance", PREFER_PERFORMANCE),
                 (50, "Interactive", PREFER_INTERACTIVE),
                 (0,  "Inactive",    PREFER_INTERACTIVE),
                 (0,  "Powersave",   PREFER_POWERSAVE),
                )

def read_values(path):
    data = open(path).read().split()
    return data

INI_DATA = {}

def secname(s):
    return "[CPUScalingGovernor%s]" % s

def add_sec(s):
    s = secname(s)
    if not s in INI_DATA:
        INI_DATA[s] = []
    return INI_DATA[s]

def add_val(s,b,p,d):
    s = add_sec(s)
    n = len(s)//2 + 1
    p = os.path.join(b, p)
    d = str(d)
    s.append("path%d=%s" % (n,p))
    s.append("data%d=%s" % (n,d))

def select(available, wanted):
    for s in wanted:
        if s in available:
            return s
    return None

def writetest(path):
    # Read data from a file and then
    # write the same data back to the file.
    ack = True
    try:
        fh = open(path)
        data = fh.read()
        fh.close()
        fh = open(path, "w")
        fh.write(data)
        fh.close()
    except:
        # Failed -> ignore the file
        ack = False
    return ack

def generate_config_data(dirpath):
    path = os.path.join(dirpath, "scaling_available_frequencies")
    frequencies = map(int, read_values(path))

    path = os.path.join(dirpath, "scaling_available_governors")
    governors = read_values(path)

    M = len(frequencies)
    for scale,name,wanted in CONFIG_BLOCKS:
        governor = select(governors, wanted)
        if not governor:
            print>>sys.stderr, "# no governor: %s" % dirpath
        else:
            add_val(name, dirpath, "scaling_governor", governor)
        maxval = frequencies[-1]
        minval = frequencies[(M-1)*scale//100]
        add_val(name, dirpath, "scaling_max_freq", maxval)
        add_val(name, dirpath, "scaling_min_freq", minval)

if __name__ == "__main__":

    # Get directories to probe from command line
    paths = sys.argv[1:]

    if not paths:
        # The logical paths are likely to be symlinks
        # with several cpus pointing to the same controls.
        #
        # Glob logical paths, generate unique set of
        # control directories and ignore directrories
        # where we can't seem to be able to write.
        hits = {}
        done = {}
        pat = "/sys/devices/system/cpu/cpu*/cpufreq"
        for d in glob.glob(pat):
            d = os.path.realpath(d)
            if d in done:
                continue
            done[d] = None
            t = os.path.join(d, "scaling_min_freq")
            if writetest(t):
                print>>sys.stderr, "# including: %s" % d
                hits[d] = None
            else:
                print>>sys.stderr, "# excluding: %s" % d
        paths = hits.keys()

    # Generate config data for each directory given/found
    for dirpath in paths:
        generate_config_data(dirpath)

    # Output results
    print "# Automatically generated MCE CPU scaling config file"
    print ""
    for scale,name,wanted in CONFIG_BLOCKS:
        s = secname(name)
        print "%s" % s
        for s in INI_DATA[s]:
            print "%s" % s
        print ""
