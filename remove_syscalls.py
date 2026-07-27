import sys, re

def process(fn):
    with open(fn) as f:
        lines = f.readlines()
    out = []
    i = 0
    removed = []
    n = len(lines)
    while i < n:
        line = lines[i]
        if re.match(r'^(SYSCALL_DEFINE|COMPAT_SYSCALL_DEFINE)\d?\(', line):
            start = i
            # find the closing brace: first line that is exactly "}\n" (col 0)
            # but first we must reach the opening body. scan forward.
            j = i
            found = False
            while j < n:
                if lines[j].rstrip('\n') == '}':
                    found = True
                    break
                j += 1
            if not found:
                print(f"WARN: no closing brace for block at {fn}:{start+1}", file=sys.stderr)
                out.append(line); i += 1; continue
            removed.append((start+1, j+1, lines[start].strip()[:70]))
            i = j + 1
            # also swallow a single trailing blank line to avoid double blanks
            if i < n and lines[i].strip() == '' and out and out[-1].strip() == '':
                i += 1
            continue
        out.append(line); i += 1
    with open(fn, 'w') as f:
        f.writelines(out)
    print(f"=== {fn}: removed {len(removed)} blocks ===")
    for s,e,t in removed:
        print(f"  lines {s}-{e}: {t}")

for fn in sys.argv[1:]:
    process(fn)
