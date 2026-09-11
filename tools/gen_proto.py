#!/usr/bin/env python3
"""Generates C++ message structs for the packets listed in proto/seeds.txt.

protoc parses the .proto set into a descriptor set; we walk it from the seeds,
close over every referenced type and emit plain structs plus wire codecs.
Generating the whole 4722-message dump would be ~150 MB of C++.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

from google.protobuf import descriptor_pb2

F = descriptor_pb2.FieldDescriptorProto

CPP_KEYWORDS = {
    "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool", "break", "case",
    "catch", "char", "class", "compl", "concept", "const", "consteval", "constexpr", "continue",
    "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
    "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
    "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
    "not", "nullptr", "operator", "or", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "throw", "true",
    "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
    "volatile", "wchar_t", "while", "xor",
}

SCALARS = {
    F.TYPE_DOUBLE: ("double", "Double", "0.0"),
    F.TYPE_FLOAT: ("float", "Float", "0.0f"),
    F.TYPE_INT64: ("int64_t", "Int64", "0"),
    F.TYPE_UINT64: ("uint64_t", "UInt64", "0"),
    F.TYPE_INT32: ("int32_t", "Int32", "0"),
    F.TYPE_FIXED64: ("uint64_t", "Fixed64", "0"),
    F.TYPE_FIXED32: ("uint32_t", "Fixed32", "0"),
    F.TYPE_BOOL: ("bool", "Bool", "false"),
    F.TYPE_STRING: ("std::string", "String", ""),
    F.TYPE_BYTES: ("std::string", "Bytes", ""),
    F.TYPE_UINT32: ("uint32_t", "UInt32", "0"),
    F.TYPE_SFIXED32: ("int32_t", "SFixed32", "0"),
    F.TYPE_SFIXED64: ("int64_t", "SFixed64", "0"),
    F.TYPE_SINT32: ("int32_t", "SInt32", "0"),
    F.TYPE_SINT64: ("int64_t", "SInt64", "0"),
}

# wire type per field type
WT = {
    F.TYPE_DOUBLE: 1, F.TYPE_FLOAT: 5, F.TYPE_INT64: 0, F.TYPE_UINT64: 0, F.TYPE_INT32: 0,
    F.TYPE_FIXED64: 1, F.TYPE_FIXED32: 5, F.TYPE_BOOL: 0, F.TYPE_STRING: 2, F.TYPE_BYTES: 2,
    F.TYPE_UINT32: 0, F.TYPE_SFIXED32: 5, F.TYPE_SFIXED64: 1, F.TYPE_SINT32: 0,
    F.TYPE_SINT64: 0, F.TYPE_ENUM: 0, F.TYPE_MESSAGE: 2, F.TYPE_GROUP: 3,
}

PACKABLE = {
    F.TYPE_DOUBLE, F.TYPE_FLOAT, F.TYPE_INT64, F.TYPE_UINT64, F.TYPE_INT32, F.TYPE_FIXED64,
    F.TYPE_FIXED32, F.TYPE_BOOL, F.TYPE_UINT32, F.TYPE_SFIXED32, F.TYPE_SFIXED64,
    F.TYPE_SINT32, F.TYPE_SINT64, F.TYPE_ENUM,
}


def cpp_name(proto_full_name):
    """.Outer.Inner -> Outer_Inner"""
    return proto_full_name.lstrip(".").replace(".", "_")


def field_name(name):
    return name + "_" if name in CPP_KEYWORDS else name


class Index:
    def __init__(self, fds):
        self.messages = {}   # ".Name" -> DescriptorProto
        self.enums = {}      # ".Name" -> EnumDescriptorProto
        for f in fds.file:
            pkg = ("." + f.package) if f.package else ""
            for m in f.message_type:
                self._add_message(pkg, m)
            for e in f.enum_type:
                self.enums[pkg + "." + e.name] = e

    def _add_message(self, prefix, m):
        full = prefix + "." + m.name
        self.messages[full] = m
        for n in m.nested_type:
            self._add_message(full, n)
        for e in m.enum_type:
            self.enums[full + "." + e.name] = e

    def is_map_entry(self, type_name):
        m = self.messages.get(type_name)
        return bool(m and m.options.map_entry)


def close_over(index, seeds):
    """Transitive closure of message/enum types reachable from the seeds."""
    msgs, enums = set(), set()
    todo = list(seeds)
    while todo:
        name = todo.pop()
        if name in msgs:
            continue
        d = index.messages.get(name)
        if d is None:
            # A seed may name an enum directly: retcodes are plain uint32 on the wire,
            # so nothing reaches Retcode through a field type.
            if name in index.enums:
                enums.add(name)
                continue
            print(f"  warning: unknown message {name}", file=sys.stderr)
            continue
        msgs.add(name)
        for fld in d.field:
            if fld.type == F.TYPE_MESSAGE or fld.type == F.TYPE_GROUP:
                todo.append(fld.type_name)
            elif fld.type == F.TYPE_ENUM:
                enums.add(fld.type_name)
    return msgs, enums


def order_messages(index, msgs):
    """Definition order: a struct must follow every message it holds by value."""
    ordered, state = [], {}

    def visit(name):
        st = state.get(name)
        if st == 2:
            return
        if st == 1:      # cycle: broken by Opt<T>'s indirection
            return
        state[name] = 1
        d = index.messages[name]
        for fld in d.field:
            if fld.type in (F.TYPE_MESSAGE, F.TYPE_GROUP) and fld.type_name in msgs:
                visit(fld.type_name)
        state[name] = 2
        ordered.append(name)

    for n in sorted(msgs):
        visit(n)
    return ordered


class Field:
    """One flattened field of a message, resolved to its C++ shape."""

    def __init__(self, index, fld, msgs):
        self.proto = fld
        self.number = fld.number
        self.name = field_name(fld.name)
        self.raw_name = fld.name
        self.type = fld.type
        self.repeated = fld.label == F.LABEL_REPEATED
        self.oneof = fld.oneof_index if fld.HasField("oneof_index") else None
        self.map = False
        self.key = None
        self.value = None

        if self.type in (F.TYPE_MESSAGE, F.TYPE_GROUP) and index.is_map_entry(fld.type_name):
            entry = index.messages[fld.type_name]
            self.map = True
            self.repeated = False
            self.key = Field(index, entry.field[0], msgs)
            self.value = Field(index, entry.field[1], msgs)
        elif self.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
            self.msg_type = cpp_name(fld.type_name)
        elif self.type == F.TYPE_ENUM:
            self.enum_type = cpp_name(fld.type_name)

    def elem_type(self):
        if self.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
            return self.msg_type
        if self.type == F.TYPE_ENUM:
            return self.enum_type
        return SCALARS[self.type][0]

    def decl_type(self):
        if self.map:
            return f"std::map<{self.key.elem_type()}, {self.value.elem_type()}>"
        if self.repeated:
            return f"std::vector<{self.elem_type()}>"
        if self.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
            return f"pb::Opt<{self.msg_type}>"
        return self.elem_type()

    def initializer(self):
        if self.map or self.repeated or self.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
            return ""
        if self.type == F.TYPE_ENUM:
            return "{}"
        return "{" + ("" if self.type in (F.TYPE_STRING, F.TYPE_BYTES) else SCALARS[self.type][2]) + "}"

    def tag(self):
        wt = 2 if (self.map or (self.repeated and self.type in PACKABLE)) else WT[self.type]
        return (self.number << 3) | wt


def gen_header(index, ordered, enums, ns):
    out = []
    w = out.append
    w("// generated by tools/gen_proto.py -- do not edit")
    w("#pragma once")
    w("#include <cstdint>")
    w("#include <map>")
    w("#include <string>")
    w("#include <vector>")
    w('#include "proto/wire.h"')
    w("")
    w(f"namespace {ns} {{")
    w("")

    for name in sorted(enums):
        e = index.enums[name]
        w(f"enum class {cpp_name(name)} : int32_t {{")
        seen = set()
        for v in e.value:
            vn = v.name
            if vn in seen:
                continue
            seen.add(vn)
            w(f"    {vn} = {v.number},")
        w("};")
        w("")

    for name in ordered:
        w(f"struct {cpp_name(name)};")
    w("")

    for name in ordered:
        d = index.messages[name]
        cn = cpp_name(name)
        fields = [Field(index, f, ordered) for f in d.field]
        w(f"struct {cn} {{")
        w(f'    static constexpr const char* kName = "{cn}";')
        for f in fields:
            w(f"    {f.decl_type()} {f.name}{f.initializer()};")
        for i, o in enumerate(d.oneof_decl):
            members = [f for f in fields if f.oneof == i]
            w(f"    enum {o.name}_case_t : int32_t {{ {o.name}_NOT_SET = 0, " +
              ", ".join(f"k_{m.name} = {m.number}" for m in members) + " };")
            w(f"    int32_t {o.name}_case{{}};")
        w("    void encode(pb::Writer& w) const;")
        w("    bool decode(pb::Reader& r);")
        # The client reads sub-messages of a response without null-checking them and
        # throws inside its own module when one is absent, so a "blank" reply often has
        # to be blank-but-present all the way down.
        w("    void fill(int depth = 1);")
        w("    std::string serialize() const { pb::Writer w; encode(w); return w.take(); }")
        w("    bool parse(const uint8_t* p, size_t n) { pb::Reader r(p, n); return decode(r); }")
        w("};")
        w("")

    w(f"}}  // namespace {ns}")
    w("")
    return "\n".join(out)


def emit_write_value(w, f, expr, indent, sink="pbw"):
    pad = " " * indent
    if f.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
        w(f"{pad}{sink}.writeMessage({expr});")
    elif f.type == F.TYPE_ENUM:
        w(f"{pad}{sink}.writeInt32(static_cast<int32_t>({expr}));")
    else:
        w(f"{pad}{sink}.write{SCALARS[f.type][1]}({expr});")


def emit_read_value(w, f, target, indent, src="pbr"):
    pad = " " * indent
    if f.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
        w(f"{pad}if (!{src}.readMessage({target})) return false;")
    elif f.type == F.TYPE_ENUM:
        w(f"{pad}{{ int32_t pbE; if (!{src}.readInt32(pbE)) return false; {target} = static_cast<{f.enum_type}>(pbE); }}")
    else:
        w(f"{pad}if (!{src}.read{SCALARS[f.type][1]}({target})) return false;")


def default_check(f, expr):
    if f.type in (F.TYPE_STRING, F.TYPE_BYTES):
        return f"!{expr}.empty()"
    if f.type == F.TYPE_BOOL:
        return expr
    if f.type == F.TYPE_ENUM:
        return f"static_cast<int32_t>({expr}) != 0"
    if f.type in (F.TYPE_DOUBLE, F.TYPE_FLOAT):
        return f"{expr} != 0"
    return f"{expr} != 0"


def gen_source(index, names, ns):
    out = []
    w = out.append
    w("// generated by tools/gen_proto.py -- do not edit")
    w('#include "proto/gen/protos.h"')
    w("")
    w(f"namespace {ns} {{")
    w("")

    for name in names:
        d = index.messages[name]
        cn = cpp_name(name)
        fields = [Field(index, f, names) for f in d.field]

        # ---- fill ----
        # Oneof members stay out of it: setting more than one of them is invalid.
        subs = [f for f in fields
                if f.type in (F.TYPE_MESSAGE, F.TYPE_GROUP)
                and not f.repeated and not f.map and f.oneof is None]
        w(f"void {cn}::fill(int depth) {{")
        if subs:
            w("    if (depth <= 0) return;")
            for f in subs:
                w(f"    {f.name}.emplace().fill(depth - 1);")
        else:
            w("    (void)depth;")
        w("}")
        w("")

        # ---- encode ----
        w(f"void {cn}::encode(pb::Writer& pbw) const {{")
        if not fields:
            w("    (void)pbw;")
        for f in fields:
            if f.map:
                key_guard = default_check(f.key, "kv.first")
                w(f"    for (const auto& kv : {f.name}) {{")
                w(f"        pbw.writeTag({f.tag()}u);")
                w("        pb::Writer pbSub;")
                w(f"        if ({key_guard}) {{ pbSub.writeTag({f.key.tag()}u); ")
                emit_write_value(w, f.key, "kv.first", 12, "pbSub")
                w("        }")
                if f.value.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
                    w(f"        pbSub.writeTag({f.value.tag()}u);")
                    emit_write_value(w, f.value, "kv.second", 8, "pbSub")
                else:
                    w(f"        if ({default_check(f.value, 'kv.second')}) {{ pbSub.writeTag({f.value.tag()}u); ")
                    emit_write_value(w, f.value, "kv.second", 12, "pbSub")
                    w("        }")
                w("        pbw.writeBytes(pbSub.data());")
                w("    }")
            elif f.repeated:
                if f.type in PACKABLE:
                    w(f"    if (!{f.name}.empty()) {{")
                    w(f"        pbw.writeTag({f.tag()}u);")
                    w("        pb::Writer pbSub;")
                    w(f"        for (const auto& pbV : {f.name}) ")
                    emit_write_value(w, f, "pbV", 12, "pbSub")
                    w("        pbw.writeBytes(pbSub.data());")
                    w("    }")
                else:
                    w(f"    for (const auto& pbV : {f.name}) {{")
                    w(f"        pbw.writeTag({f.tag()}u);")
                    emit_write_value(w, f, "pbV", 8)
                    w("    }")
            elif f.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
                w(f"    if ({f.name}) {{ pbw.writeTag({f.tag()}u); pbw.writeMessage(*{f.name}); }}")
            elif f.oneof is not None:
                oname = d.oneof_decl[f.oneof].name
                w(f"    if ({oname}_case == {f.number}) {{ pbw.writeTag({f.tag()}u); ")
                emit_write_value(w, f, f.name, 8)
                w("    }")
            else:
                w(f"    if ({default_check(f, f.name)}) {{ pbw.writeTag({f.tag()}u); ")
                emit_write_value(w, f, f.name, 8)
                w("    }")
        w("}")
        w("")

        # ---- decode ----
        w(f"bool {cn}::decode(pb::Reader& pbr) {{")
        w("    while (!pbr.eof()) {")
        w("        uint32_t pbTag;")
        w("        if (!pbr.readTag(pbTag)) return false;")
        w("        switch (pbTag) {")
        for f in fields:
            if f.map:
                w(f"        case {f.tag()}u: {{")
                w("            pb::Reader pbSub;")
                w("            if (!pbr.readSub(pbSub)) return false;")
                w(f"            {f.key.elem_type()} pbK{f.key.initializer() or '{}'};")
                w(f"            {f.value.elem_type()} pbVal{f.value.initializer() or '{}'};")
                w("            while (!pbSub.eof()) {")
                w("                uint32_t pbT2;")
                w("                if (!pbSub.readTag(pbT2)) return false;")
                w(f"                if (pbT2 == {f.key.tag()}u) {{")
                emit_read_value(w, f.key, "pbK", 20, "pbSub")
                w("                } else if (pbT2 == %du) {" % f.value.tag())
                emit_read_value(w, f.value, "pbVal", 20, "pbSub")
                w("                } else if (!pbSub.skip(pbT2)) return false;")
                w("            }")
                w(f"            {f.name}[pbK] = std::move(pbVal);")
                w("            break;")
                w("        }")
            elif f.repeated:
                if f.type in PACKABLE:
                    w(f"        case {f.tag()}u: {{")
                    w("            pb::Reader pbSub;")
                    w("            if (!pbr.readSub(pbSub)) return false;")
                    w("            while (!pbSub.eof()) {")
                    w(f"                {f.elem_type()} pbV{{}};")
                    emit_read_value(w, f, "pbV", 16, "pbSub")
                    w(f"                {f.name}.push_back(pbV);")
                    w("            }")
                    w("            break;")
                    w("        }")
                    unpacked = (f.number << 3) | WT[f.type]
                    w(f"        case {unpacked}u: {{")
                    w(f"            {f.elem_type()} pbV{{}};")
                    emit_read_value(w, f, "pbV", 12)
                    w(f"            {f.name}.push_back(pbV);")
                    w("            break;")
                    w("        }")
                else:
                    w(f"        case {f.tag()}u: {{")
                    w(f"            {f.elem_type()} pbV{{}};")
                    emit_read_value(w, f, "pbV", 12)
                    w(f"            {f.name}.push_back(std::move(pbV));")
                    w("            break;")
                    w("        }")
            elif f.type in (F.TYPE_MESSAGE, F.TYPE_GROUP):
                w(f"        case {f.tag()}u:")
                w(f"            if (!pbr.readMessage({f.name}.emplace())) return false;")
                if f.oneof is not None:
                    w(f"            {d.oneof_decl[f.oneof].name}_case = {f.number};")
                w("            break;")
            else:
                w(f"        case {f.tag()}u:")
                emit_read_value(w, f, f.name, 12)
                if f.oneof is not None:
                    w(f"            {d.oneof_decl[f.oneof].name}_case = {f.number};")
                w("            break;")
        w("        default:")
        w("            if (!pbr.skip(pbTag)) return false;")
        w("            break;")
        w("        }")
        w("    }")
        w("    return true;")
        w("}")
        w("")

    w(f"}}  // namespace {ns}")
    w("")
    return "\n".join(out)


def parse_seeds(path):
    seeds = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                seeds.append(line)
    return seeds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proto-dir", required=True)
    ap.add_argument("--seeds", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--protoc", default="protoc")
    ap.add_argument("--shards", type=int, default=8)
    ap.add_argument("--namespace", default="proto")
    args = ap.parse_args()

    protos = sorted(f for f in os.listdir(args.proto_dir) if f.endswith(".proto"))
    tmp = tempfile.mkdtemp(prefix="capysr-proto-")
    desc = os.path.join(tmp, "all.desc")
    try:
        subprocess.run(
            [args.protoc, f"--descriptor_set_out={desc}", "-I", args.proto_dir] + protos,
            check=True,
        )
        fds = descriptor_pb2.FileDescriptorSet()
        with open(desc, "rb") as fh:
            fds.ParseFromString(fh.read())
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    index = Index(fds)
    seeds = ["." + s for s in parse_seeds(args.seeds)]
    msgs, enums = close_over(index, seeds)
    ordered = order_messages(index, msgs)
    print(f"  {len(ordered)} messages, {len(enums)} enums from {len(seeds)} seeds")

    os.makedirs(args.out_dir, exist_ok=True)
    header = gen_header(index, ordered, enums, args.namespace)
    write_if_changed(os.path.join(args.out_dir, "protos.h"), header)

    shards = [[] for _ in range(args.shards)]
    for i, name in enumerate(ordered):
        shards[i % args.shards].append(name)
    for i, names in enumerate(shards):
        src = gen_source(index, names, args.namespace)
        write_if_changed(os.path.join(args.out_dir, f"protos_{i}.cpp"), src)


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path, encoding="utf-8") as fh:
            if fh.read() == text:
                return
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


if __name__ == "__main__":
    main()
