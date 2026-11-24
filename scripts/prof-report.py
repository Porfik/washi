# Разбор профиля prof.c: python3 -I prof-report.py NM_FILE PROF... (nm -n той сборки, что работала).
# Для каждого файла и суммарно: доля сэмплов по функциям; для кода вне программы (libc и т.п.) —
# по вызывающей функции (адрес возврата).
import sys, re, bisect, collections

nm_path, profs = sys.argv[1], sys.argv[2:]
addrs, names = [], []
for line in open(nm_path, encoding='utf-8', errors='replace'):
    p = line.split()
    if len(p) >= 3 and p[1] in 'tTwW':
        addrs.append(int(p[0], 16)); names.append(p[2])

def sym(a):
    i = bisect.bisect_right(addrs, a) - 1
    return names[i] if i >= 0 else '?'

def load(path):
    maps, rows = [], []
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('#'):
            continue
        m = re.match(r'([0-9a-f]+)-([0-9a-f]+) r-xp ([0-9a-f]+) \S+ \S+\s*(.*)', line)
        if m:
            maps.append((int(m.group(1), 16), int(m.group(2), 16), m.group(4).strip(), int(m.group(3), 16)))
            continue
        p = line.split()
        if len(p) == 3:
            rows.append((int(p[0]), int(p[1], 16), int(p[2], 16)))
    main = maps[0] if maps else (0, 0, '', 0)
    # Сборка для книги — обычный исполняемый файл (адреса как в nm), на ПК — PIE (сдвиг загрузки).
    shift = main[0] - main[3] if main[0] >= 1 << 32 else 0
    def where(a):
        for lo, hi, name, off in maps:
            if lo <= a < hi:
                return name
        return '?'
    c = collections.Counter()
    for n, pc, lr in rows:
        if main[0] <= pc < main[1]:
            c[sym(pc - shift)] += n
        else:
            lib = where(pc).rsplit('/', 1)[-1] or '?'
            caller = sym(lr - shift) if main[0] <= lr < main[1] else '?'
            c['[%s] из %s' % (lib, caller)] += n
    return c

total = collections.Counter(); per = {}
for path in profs:
    c = load(path)
    per[path.rsplit('/', 1)[-1].rsplit('.', 1)[0]] = c
    total.update(c)

def show(title, c, top):
    s = sum(c.values()) or 1
    print('== %s: %d сэмплов' % (title, s))
    for fn, n in c.most_common(top):
        print('%5.1f%%  %s' % (100 * n / s, fn))

if len(per) > 1:
    show('всё вместе (каждая книга с весом её сэмплов)', total, 35)
    print()
for name, c in per.items():
    show(name, c, 12)
    print()
