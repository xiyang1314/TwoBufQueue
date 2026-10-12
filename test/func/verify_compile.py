"""Require a valid control to compile and the invalid case to fail its static_assert."""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

HEADER = '#include "TwoBufQueue/src/write/writer.hpp"\n#include <string>\nusing namespace TwoBufQueue;\n'
CONTROL = (
    'void f(WriterQueue* p){Writer<1> w(p,0);(void)w.write(std::uint32_t{1});}'
    'struct Large {char bytes[65536];};'
    'void wide(WriterQueue* p){Writer<1,true,std::uint32_t> w(p,0);Large v{};(void)w.write(v);}'
    'void raw(WriterQueue* p){Writer<1,false> w(p,0);Large v{};(void)w.write(v);}'
)
CASES = {
    "T_pointer": (
        'void f(WriterQueue* p){Writer<> w(p,0);unsigned n=0;auto v=&n;(void)w.write(v);}',
        'use write_mem for pointer/array payloads',
    ),
    "T_array": (
        'void f(WriterQueue* p){Writer<> w(p,0);char v[4]{};(void)w.write(v);}',
        'use write_mem for pointer/array payloads',
    ),
    "T_nontrivial": (
        'void f(WriterQueue* p){Writer<> w(p,0);std::string v;(void)w.write(v);}',
        'write requires trivially-copyable arguments',
    ),
    "T_maximum": (
        'struct Large {char data[65536];};void f(WriterQueue* p){Writer<> w(p,0);Large v{};(void)w.write(v);}',
        'payload does not fit TDataLen',
    ),
    "T_publish0": (
        'void f(WriterQueue* p){Writer<0> w(p,0);}',
        'Writer publish_write_ptr_count must be positive',
    ),
    'T_len_signed': ('Writer<1,true,int> writer(nullptr,0);', 'TDataLen must be an unqualified unsigned integer of at most 64 bits'),
    'T_len_bool': ('Writer<1,true,bool> writer(nullptr,0);', 'TDataLen must be an unqualified unsigned integer of at most 64 bits'),
    'T_len_float': ('Writer<1,true,double> writer(nullptr,0);', 'TDataLen must be an unqualified unsigned integer of at most 64 bits'),
    'T_len_const': ('Writer<1,true,const unsigned> writer(nullptr,0);', 'TDataLen must be an unqualified unsigned integer of at most 64 bits'),
    'T_len_u8_payload': ('struct Large {char data[256];}; void f(WriterQueue* p){Writer<1,true,std::uint8_t> w(p,0);Large v{};(void)w.write(v);}', 'payload does not fit TDataLen'),
    'T_raw_empty': ('void f(WriterQueue* p){Writer<1,false> w(p,0);(void)w.write();}', 'unframed payload must not be empty'),
}


def run(command, timeout=60):
    return subprocess.run(
        command, capture_output=True, text=True, errors="replace", timeout=timeout
    )


def configure_msvc(args, directory):
    # The VS generator supplies the SDK environment even outside a Developer shell.
    include = Path(args.include).as_posix()
    project = (
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(CompileContract LANGUAGES CXX)\n"
        "set(CMAKE_CXX_STANDARD 20)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        # Match the main MSVC targets when parsing UTF-8 headers with Chinese comments.
        "add_compile_options(/utf-8)\n"
        "add_library(valid OBJECT valid.cpp)\n"
        "add_library(invalid OBJECT invalid.cpp)\n"
        f"target_include_directories(valid PRIVATE [[{include}]])\n"
        f"target_include_directories(invalid PRIVATE [[{include}]])\n"
    )
    (directory / "CMakeLists.txt").write_text(project, encoding="utf-8")
    command = [args.cmake, "-S", str(directory), "-B", str(directory / "build"),
               "-G", args.generator, "-DCMAKE_CXX_COMPILER=" + args.compiler]
    if args.platform:
        command += ["-A", args.platform]
    if args.toolset:
        command += ["-T", args.toolset]
    result = run(command)
    if result.returncode:
        sys.exit("Control configure failed:\n" + result.stdout + result.stderr)


def compile_source(args, directory, name):
    if args.msvc == "1":
        return run([args.cmake, "--build", str(directory / "build"),
                    "--config", "Release", "--target", name])
    return run([args.compiler, "-std=c++20", "-c", str(directory / (name + ".cpp")),
                "-I", args.include, "-o", str(directory / (name + ".o"))], timeout=45)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--msvc", choices=("0", "1"), default="0")
    parser.add_argument("--case", required=True, choices=CASES)
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--generator", default="")
    parser.add_argument("--platform", default="")
    parser.add_argument("--toolset", default="")
    args = parser.parse_args()
    body, diagnostic = CASES[args.case]
    with tempfile.TemporaryDirectory(prefix="brq-compile-") as temp:
        directory = Path(temp)
        (directory / "valid.cpp").write_text(HEADER + CONTROL, encoding="utf-8")
        (directory / "invalid.cpp").write_text(HEADER + body, encoding="utf-8")
        if args.msvc == "1":
            configure_msvc(args, directory)
        good = compile_source(args, directory, "valid")
        if good.returncode:
            sys.exit("Valid control failed:\n" + good.stdout + good.stderr)
        bad = compile_source(args, directory, "invalid")
        if bad.returncode == 0 or diagnostic not in bad.stdout + bad.stderr:
            sys.exit("Expected static_assert not observed:\n" + bad.stdout + bad.stderr)
    print("PASS", args.case, "valid control and exact diagnostic")


if __name__ == "__main__":
    main()
