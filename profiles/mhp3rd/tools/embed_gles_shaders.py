#!/usr/bin/env python3
"""Embed the GLES2 shader sources without changing their contents."""
import pathlib
import sys
source, output = map(pathlib.Path, sys.argv[1:])
output.parent.mkdir(parents=True, exist_ok=True)
text = ""
for name, filename in [("kGlesVertex", "ge_gles2.vert"), ("kGlesFragment", "ge_gles2.frag")]:
    shader = (source / filename).read_text()
    text += 'static const char *' + name + ' = R"GLSL(' + shader + ')GLSL";\n'
if not output.exists() or output.read_text() != text:
    output.write_text(text)
