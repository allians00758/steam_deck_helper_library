#!/usr/bin/env python3
"""Build-only SDH bridge insertion, from a separate GitHub branch."""
from pathlib import Path
import sys
root=Path(sys.argv[1])
vcx=root/"OptiScaler"/"OptiScaler.vcxproj"
s=vcx.read_text(encoding="utf-8-sig")
needle='  </ItemGroup>'
i=s.find('<ClCompile Include=')
if i<0:raise SystemExit("Missing ClCompile elements")
end=s.find("</ItemGroup>",i)
if end<0:raise SystemExit("Missing closing ItemGroup")
if 'SDHControlBridge.cpp' in s:raise SystemExit("Bridge already exists in project")
s=s[:end]+'    <ClCompile Include="SDHControlBridge.cpp" />\n  '+s[end:]
vcx.write_text(s,encoding="utf-8-sig")
print("SOURCE_PATCH=PASS")
