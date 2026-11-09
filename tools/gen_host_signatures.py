#!/usr/bin/env python3
"""Writes src/host_signatures.c: which arguments of each host function the game calls are
pointers, so guest-base builds can translate them (see include/guest_memory.h).

A signature is "<result>:<arguments>", one letter each: i = a 32-bit word, p = a pointer (a guest
address the host sees as base + address), s = a string result (copied into guest memory when the
host's own string lies outside it), l = a 64-bit result (r0:r1), v = no result.

Sources, later ones winning for the same name:
  1. the GLES 2 headers SDL bundles (gl2.h, gl2ext.h) and the GLES 1 list below;
  2. every function defined in src/s3e_*.c: the s3e API, the extension interfaces, and the
     host_<name> wrappers (recorded as <name>).

Run it after adding or changing a host function: python3 tools/gen_host_signatures.py
"""
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Types that are pointers without a '*' in their spelling.
POINTER_TYPES = {
    'GLeglImageOES', 'GLsync', 'GLDEBUGPROCKHR', 'GLDEBUGPROC', 'EGLDisplay', 'EGLConfig',
    'EGLSurface', 'EGLContext', 'EGLNativeWindowType', 'EGLNativeDisplayType', 'EGLClientBuffer',
    's3e_callback_fn', 's3e_socket_callback_fn', 's3e_zeroconf_callback_fn',
    'audio_unit_callback_fn', 'FILE',
}
WIDE_TYPES = {'uint64_t', 'int64_t', 'GLuint64', 'GLint64'}

# GLES 1 functions with pointer arguments (SDL does not bundle GLES/gl.h). Everything else in
# GLES 1 takes words.
GLES1 = {
    'glClipPlanef': 'v:ip', 'glClipPlanex': 'v:ip', 'glClipPlanexOES': 'v:ip',
    'glClipPlanefOES': 'v:ip', 'glClipPlanefIMG': 'v:ip', 'glClipPlanexIMG': 'v:ip',
    'glColorPointer': 'v:iiip', 'glNormalPointer': 'v:iip', 'glTexCoordPointer': 'v:iiip',
    'glVertexPointer': 'v:iiip', 'glMatrixIndexPointerOES': 'v:iiip',
    'glWeightPointerOES': 'v:iiip', 'glPointSizePointerOES': 'v:iip',
    'glDrawTexfvOES': 'v:p', 'glDrawTexivOES': 'v:p', 'glDrawTexsvOES': 'v:p',
    'glDrawTexxvOES': 'v:p',
    'glFogfv': 'v:ip', 'glFogxv': 'v:ip', 'glFogxvOES': 'v:ip',
    'glGetClipPlanefOES': 'v:ip', 'glGetClipPlanexOES': 'v:ip', 'glGetFixedvOES': 'v:ip',
    'glGetLightxvOES': 'v:iip', 'glGetMaterialxvOES': 'v:iip', 'glGetTexEnvxvOES': 'v:iip',
    'glGetTexGenfvOES': 'v:iip', 'glGetTexGenivOES': 'v:iip', 'glGetTexGenxvOES': 'v:iip',
    'glGetTexParameterxvOES': 'v:iip', 'glGetPointerv': 'v:ip',
    'glLightModelfv': 'v:ip', 'glLightModelxv': 'v:ip', 'glLightModelxvOES': 'v:ip',
    'glLightfv': 'v:iip', 'glLightxv': 'v:iip', 'glLightxvOES': 'v:iip',
    'glLoadMatrixf': 'v:p', 'glLoadMatrixx': 'v:p', 'glLoadMatrixxOES': 'v:p',
    'glMaterialfv': 'v:iip', 'glMaterialxv': 'v:iip', 'glMaterialxvOES': 'v:iip',
    'glMultMatrixf': 'v:p', 'glMultMatrixx': 'v:p', 'glMultMatrixxOES': 'v:p',
    'glPointParameterfv': 'v:ip', 'glPointParameterxv': 'v:ip', 'glPointParameterxvOES': 'v:ip',
    'glQueryMatrixxOES': 'i:pp',
    'glTexEnvfv': 'v:iip', 'glTexEnviv': 'v:iip', 'glTexEnvxv': 'v:iip', 'glTexEnvxvOES': 'v:iip',
    'glTexGenfvOES': 'v:iip', 'glTexGenivOES': 'v:iip', 'glTexGenxvOES': 'v:iip',
    'glTexParameterxv': 'v:iip', 'glTexParameterxvOES': 'v:iip',
    'glGetLightfv': 'v:iip', 'glGetMaterialfv': 'v:iip', 'glGetTexEnvfv': 'v:iip',
    'glGetTexEnviv': 'v:iip', 'glGetLightxv': 'v:iip', 'glGetMaterialxv': 'v:iip',
    'glGetTexEnvxv': 'v:iip', 'glGetTexParameterxv': 'v:iip', 'glGetFixedv': 'v:ip',
    'glGetClipPlanef': 'v:ip', 'glGetClipPlanex': 'v:ip',
}

# GLES 3.x functions the game imports (SDL bundles only the GLES 2 headers). Those not listed
# take words only.
GLES3 = {
    'glDrawBuffers': 'v:ip', 'glReadBuffer': 'v:i',
    'glGenQueries': 'v:ip', 'glDeleteQueries': 'v:ip', 'glGetQueryiv': 'v:iip',
    'glGetQueryObjectuiv': 'v:iip',
    'glGenVertexArrays': 'v:ip', 'glDeleteVertexArrays': 'v:ip',
    'glGenSamplers': 'v:ip', 'glDeleteSamplers': 'v:ip', 'glSamplerParameteriv': 'v:iip',
    'glSamplerParameterfv': 'v:iip',
    'glGenProgramPipelines': 'v:ip', 'glDeleteProgramPipelines': 'v:ip',
    'glGenTransformFeedbacks': 'v:ip', 'glDeleteTransformFeedbacks': 'v:ip',
    'glUniform1uiv': 'v:iip', 'glUniform2uiv': 'v:iip', 'glUniform3uiv': 'v:iip',
    'glUniform4uiv': 'v:iip',
    'glUniformMatrix2x3fv': 'v:iiip', 'glUniformMatrix3x2fv': 'v:iiip',
    'glUniformMatrix2x4fv': 'v:iiip', 'glUniformMatrix4x2fv': 'v:iiip',
    'glUniformMatrix3x4fv': 'v:iiip', 'glUniformMatrix4x3fv': 'v:iiip',
    'glTexImage3D': 'v:iiiiiiiiip', 'glTexSubImage3D': 'v:iiiiiiiiiip',
    'glCompressedTexImage3D': 'v:iiiiiiiip', 'glCompressedTexSubImage3D': 'v:iiiiiiiiiip',
    'glVertexAttribIPointer': 'v:iiiip', 'glGetUniformIndices': 'v:iipp',
    'glMapBufferRange': 'p:iiii', 'glFenceSync': 'p:ii', 'glDeleteSync': 'v:p',
    'glClientWaitSync': 'i:pii', 'glWaitSync': 'v:pii',
}


def kind(declaration, result=False):
    text = declaration.strip()
    if not text or text == 'void':
        return 'v' if result else ''
    if result and re.search(r'\b(char|GLchar|GLubyte)\b', text) and text.count('*') == 1:
        return 's'  # a string: copied into guest memory when the host's lies outside it
    if '*' in text or '[' in text:
        return 'p'
    words = re.findall(r'[A-Za-z_]\w*', text)
    if any(w in POINTER_TYPES for w in words):
        return 'p'
    if any(w in WIDE_TYPES for w in words):
        return 'l' if result else 'ii'  # a 64-bit argument: two words (not used by any import)
    return 'i'


def split_params(params):
    params = ' '.join(params.split())
    if params in ('', 'void'):
        return []
    return [p for p in params.split(',')]


def signature(result, params):
    return kind(result, True) + ':' + ''.join(kind(p) for p in split_params(params))


def main():
    table = {}
    for header in ('SDL_opengles2_gl2.h', 'SDL_opengles2_gl2ext.h'):
        text = open(os.path.join(ROOT, 'third_party/SDL2/include', header), encoding='utf-8').read()
        for m in re.finditer(r'GL_APICALL\s+(.+?)\s+GL_APIENTRY\s+(\w+)\s*\(([^)]*)\)\s*;', text):
            table[m.group(2)] = signature(m.group(1), m.group(3))
    table.update(GLES1)
    table.update(GLES3)

    definition = re.compile(
        r'^(?:static\s+)?(?:S3E_SOFTFP\s+)?((?:const\s+)?[A-Za-z_][\w]*(?:\s*\*+)?)\s*'
        r'(?:S3E_SOFTFP\s+)?\b(\w+)\s*\(([^;{)]*)\)\s*\{', re.M)
    for path in sorted(glob.glob(os.path.join(ROOT, 'src', 's3e_*.c'))):
        text = open(path, encoding='utf-8').read()
        # GL_WRAP_FLOATn(name, ...): n words, no result.
        for m in re.finditer(r'GL_WRAP_FLOAT(\d)(?:_ALIAS)?\((\w+),', text):
            table[m.group(2)] = 'v:' + 'i' * int(m.group(1))
        for m in definition.finditer(text):
            result, name, params = m.group(1), m.group(2), m.group(3)
            if result in ('return', 'else', 'if', 'while', 'for', 'switch'):
                continue
            if name.startswith('host_'):
                name = name[len('host_'):]
            elif not re.match(r'(s3e|is[A-Z]|is_)', name):
                continue
            table[name] = signature(result, params)

    out = ['/* Generated by tools/gen_host_signatures.py: do not edit. */',
           '#include "host_signatures.h"', '', '#include <stdlib.h>', '#include <string.h>', '',
           'static const struct host_signature SIGNATURES[] = {']
    for name in sorted(table):
        out.append(f'    {{"{name}", "{table[name]}"}},')
    out += ['};', '',
            'static int compare(const void *key, const void *entry) {',
            '    return strcmp(key, ((const struct host_signature *)entry)->name);',
            '}', '',
            'const char *host_signature(const char *name) {',
            '    const struct host_signature *found =',
            '        bsearch(name, SIGNATURES, sizeof(SIGNATURES) / sizeof(SIGNATURES[0]),',
            '                sizeof(SIGNATURES[0]), compare);',
            '    return found ? found->signature : NULL;',
            '}', '']
    path = os.path.join(ROOT, 'src', 'host_signatures.c')
    open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
    print(f'{path}: {len(table)} functions', file=sys.stderr)


if __name__ == '__main__':
    main()
