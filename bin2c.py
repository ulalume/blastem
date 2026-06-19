#!/usr/bin/env python3
# Emit a C source file with `const unsigned char NAME[]` + `const unsigned int NAME_len`
# from a binary file, so it can be embedded in the executable.
#   Usage: bin2c.py <input-binary> <symbol-name> <output.c>
import sys

with open(sys.argv[1], 'rb') as f:
    data = f.read()
name = sys.argv[2]
with open(sys.argv[3], 'w') as out:
    out.write('const unsigned char %s[] = {\n' % name)
    for i in range(0, len(data), 16):
        out.write('\t' + ','.join('%d' % b for b in data[i:i+16]) + ',\n')
    out.write('};\n')
    out.write('const unsigned int %s_len = %u;\n' % (name, len(data)))
