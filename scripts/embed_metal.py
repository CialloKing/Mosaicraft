"""Embed an offline metallib without runtime source compilation."""
import pathlib
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
source = "#pragma once\nnamespace mosaicraft::compute {\ninline constexpr unsigned char metalLibrary[] = {\n"
source += "\n".join(",".join(str(v) for v in data[i:i + 24]) + "," for i in range(0, len(data), 24))
source += "\n};\n}\n"
pathlib.Path(sys.argv[2]).write_text(source, encoding="utf-8")
