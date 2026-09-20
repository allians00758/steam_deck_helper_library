import re, sys
from pathlib import Path
p=Path(sys.argv[1])
b=p.read_bytes()
vals=[]
for enc, pat in [("ascii", rb"[ -~]{6,}"), ("utf16", rb"(?:[ -~]\\x00){6,}")]:
    for m in re.finditer(pat,b):
        raw=m.group(0)
        try:
            s=raw.decode("ascii" if enc=="ascii" else "utf-16le", errors="ignore")
        except Exception:
            continue
        sl=s.lower()
        if any(k in sl for k in ["0.9.5", "commit", "build", "pre", "optiscaler", "202609", "2026-09"]):
            vals.append((m.start(),enc,s))
with open(sys.argv[2],"w",encoding="utf-8") as f:
    for off,enc,s in vals:
        f.write(f"0x{off:x} [{enc}] {s}\\n")
