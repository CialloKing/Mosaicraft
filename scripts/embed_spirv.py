"""Embed offline shaders so installed binaries do not depend on build paths."""
import pathlib
import struct
import sys

output = pathlib.Path(sys.argv[1])
parts = ["#pragma once\n#include <cstdint>\nnamespace mosaicraft::compute {\n"]
for name, path in zip(("featureSpirv", "scoreSpirv"), sys.argv[2:]):
    data = pathlib.Path(path).read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    parts.append(f"inline constexpr uint32_t {name}[] = {{\n")
    parts.extend(",".join(hex(v) for v in words[i:i + 12]) + ",\n" for i in range(0, len(words), 12))
    parts.append("};\n")
parts.append("}\n")
output.write_text("".join(parts), encoding="utf-8")
