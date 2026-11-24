# Сводка callgrind_annotate --inclusive=no по нескольким файлам: доля функций в сумме и по книгам.
#   python3 -I scripts/cg-sum.py DIR/*.ann   (DIR — результат scripts/corpus-prof.sh)
import sys, re, collections
tot = collections.Counter(); per = {}
for path in sys.argv[1:]:
    name = path.rsplit('/', 1)[-1].rsplit('.', 1)[0]
    c = collections.Counter(); total = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'\s*([\d,]+) \([ \d.]+%\)\s+(\S+?):(\S+)', line)
        if m:
            fn = m.group(3); n = int(m.group(1).replace(',', ''))
            c[fn] += n
        m = re.match(r'\s*([\d,]+) \([ \d.]+%\)\s+PROGRAM TOTALS', line)
        if m: total = int(m.group(1).replace(',', ''))
    per[name] = (total, c)
    tot.update(c)
grand = sum(t for t, _ in per.values())
print('всего %.1f млрд инструкций' % (grand / 1e9))
for fn, n in tot.most_common(30):
    books = sorted(((c[fn] / t, b) for b, (t, c) in per.items() if c[fn] / t > 0.05), reverse=True)
    print('%5.1f%%  %-48s %s' % (100 * n / grand, fn[:48], ', '.join('%s %d%%' % (b, 100 * f) for f, b in books[:6])))
