import hashlib, sys
import pefile

def sections(path):
    pe = pefile.PE(path, fast_load=False)
    out = {}
    for sec in pe.sections:
        name = sec.Name.rstrip(b"\\0").decode("ascii", "replace")
        data = sec.get_data()
        out[name] = (len(data), hashlib.sha256(data).hexdigest())
    return out

def main():
    target, candidate, output = sys.argv[1:4]
    a = sections(target)
    b = sections(candidate)
    with open(output, "w", encoding="utf-8") as f:
        f.write(f"TARGET={target}\\nCANDIDATE={candidate}\\n")
        for name in sorted(set(a) | set(b)):
            f.write(f"{name}: target={a.get(name)} candidate={b.get(name)} match={a.get(name) == b.get(name)}\\n")

if __name__ == "__main__":
    main()
