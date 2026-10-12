"""Record compiler and complete queue initialization for fixed-parameter replay."""
import os
import pathlib
import re
import shutil
import json


def compiler_info(executable):
    cache=pathlib.Path(executable).parent/'CMakeCache.txt'
    info={}
    if cache.exists():
        for line in cache.read_text().splitlines():
            if not line or line.startswith(('#','//')) or '=' not in line:continue
            key,value=line.split('=',1);name=key.split(':')[0]
            if name in ('CMAKE_CXX_COMPILER','CMAKE_CXX_FLAGS','CMAKE_CXX_FLAGS_RELEASE',
                        'CMAKE_BUILD_TYPE','BRQ_ENABLE_POOL_DEPTH_STATS'):
                info[name]=value
        for file in cache.parent.glob('CMakeFiles/*/CMakeCXXCompiler.cmake'):
            text=file.read_text()
            for name in ('CMAKE_CXX_COMPILER_ID','CMAKE_CXX_COMPILER_VERSION'):
                match=re.search(r'set\('+name+r' "([^\"]+)"\)',text)
                if match:info[name]=match.group(1)
    return info


def choose_compiler(explicit=None,profile=None):
    if explicit:return explicit
    recorded=(profile or {}).get('build',{}).get('CMAKE_CXX_COMPILER')
    if recorded:
        if pathlib.Path(recorded).exists():return recorded
        raise ValueError('recorded compiler unavailable; install it or explicitly select--compiler')
    for candidate in ('/opt/homebrew/opt/llvm@18/bin/clang++',shutil.which('clang++-18')):
        if candidate and pathlib.Path(candidate).exists() and os.access(candidate,os.X_OK):return candidate
    return None


def capture_queue_parameters(folder,business,labels):
    return {label:json.loads((pathlib.Path(folder)/f'raw/{business}k_{label}_run1.json').read_text())['queue_config']
            for label in labels}


def append_queue_parameters(command,parameters):
    result=list(command)
    for key,value in (parameters or {}).items():
        if key=='transport':continue  # baseline is an outer benchmark mode.
        result.extend(['--'+key,str(value).lower() if isinstance(value,bool) else str(value)])
    return result
