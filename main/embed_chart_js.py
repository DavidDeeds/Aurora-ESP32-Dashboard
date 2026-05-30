from pathlib import Path

data = Path("chart.umd.min.js").read_bytes()
Path("chart_umd_min_js.h").write_text(
    "#pragma once\n"
    "#include <stddef.h>\n"
    "extern const unsigned char chart_umd_min_js[];\n"
    "extern const size_t chart_umd_min_js_len;\n",
    encoding="utf-8",
)
lines = ['#include "chart_umd_min_js.h"', "", "const unsigned char chart_umd_min_js[] = {"]
for i in range(0, len(data), 16):
    chunk = data[i : i + 16]
    lines.append("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",")
lines[-1] = lines[-1].rstrip(",")
lines.append("};")
lines.append("")
lines.append("const size_t chart_umd_min_js_len = sizeof(chart_umd_min_js);")
lines.append("")
Path("chart_umd_min_js.c").write_text("\n".join(lines), encoding="utf-8")
print("bytes", len(data))
