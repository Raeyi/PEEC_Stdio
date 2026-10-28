# -*- coding: utf-8 -*-
"""Static validation for task_layer: JSON config parse + include existence check."""
import glob
import json
import os
import re

base = os.path.dirname(os.path.abspath(__file__))

# 1) JSON config syntax
for f in ["config/modules.json", "config/pipelines.json"]:
    path = os.path.join(base, f)
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    print("OK", os.path.basename(path), "schemaVersion=", data["schemaVersion"])
    print("   modules:", len(data.get("modules", {})), "pipelines:", len(data.get("pipelines", {})))

# 2) include existence
missing = []
files = glob.glob(base + "/src/**/*.h", recursive=True) + \
         glob.glob(base + "/src/**/*.cpp", recursive=True) + \
         glob.glob(base + "/worker_host/*.cpp")
for f in files:
    with open(f, encoding="utf-8") as fh:
        src = fh.read()
    d = os.path.dirname(f)
    for m in re.finditer(r'#include "([^"]+)"', src):
        inc = m.group(1)
        if not os.path.exists(os.path.join(d, inc)) and \
           not os.path.exists(os.path.join(base, "src", inc)):
            missing.append((os.path.relpath(f, base), inc))
for f, inc in missing:
    print("MISSING:", f, "->", inc)
print("include check done,", len(missing), "missing")

# 3) cross-reference: pipeline stage module ids must exist in modules registry
with open(os.path.join(base, "config/modules.json"), encoding="utf-8") as fh:
    mods = json.load(fh)["modules"]
with open(os.path.join(base, "config/pipelines.json"), encoding="utf-8") as fh:
    pipes = json.load(fh)["pipelines"]
bad = []
for pid, p in pipes.items():
    for s in p["stages"]:
        if s["module"] not in mods:
            bad.append((pid, s["id"], s["module"]))
for pid, sid, mid in bad:
    print("BROKEN REF:", pid, sid, "->", mid)
print("pipeline-module cross check done,", len(bad), "broken")
