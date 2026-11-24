#!/bin/sh
# Сведения о системе книги для хаба. Запуск: строка «shell | /mnt/ext1/washi/probe.sh»
# в bench.txt (washi-bench). Только читает систему; пишет в /mnt/ext1/washi/probe/.
OUT=/mnt/ext1/washi/probe
mkdir -p "$OUT/fw" "$OUT/etc"
exec > "$OUT/probe.txt" 2>&1

sec() { echo; echo "==== $*"; }

# Процессы: pid, имя, состояние, ppid, utime, stime (такты), rss (страницы), uid, cmdline.
procs() {
    for d in /proc/[0-9]*; do
        [ -r "$d/stat" ] || continue
        st=$(cat "$d/stat" 2>/dev/null) || continue
        uid=$(sed -n 's/^Uid:[[:space:]]*\([0-9]*\).*/\1/p' "$d/status" 2>/dev/null)
        cmd=$(tr '\0' ' ' < "$d/cmdline" 2>/dev/null | cut -c1-160)
        # После «)» поля: 3 state, 4 ppid, 14 utime, 15 stime, 24 rss.
        rest=${st#*) }
        name=${st#*(}
        name=${name%%)*}
        set -- $rest
        echo "$(basename "$d") $name $1 ppid=$2 ut=${12} st=${13} rss=${22} uid=$uid | $cmd"
    done
}

sec "система"
date
id
uname -a
cat /proc/version
cat /proc/cmdline
uptime
cat /proc/loadavg
grep -E 'MemTotal|MemFree|Buffers|^Cached|SwapTotal' /proc/meminfo

sec "процессы (1)"
procs > "$OUT/procs1.txt"
cat "$OUT/procs1.txt"
head -1 /proc/stat > "$OUT/stat1.txt"

sec "частоты процессора, время в каждой (10 мс)"
for c in /sys/devices/system/cpu/cpu[0-9]*; do
    echo "$c: online=$(cat $c/online 2>/dev/null) gov=$(cat $c/cpufreq/scaling_governor 2>/dev/null) cur=$(cat $c/cpufreq/scaling_cur_freq 2>/dev/null)"
    cat "$c/cpufreq/stats/time_in_state" 2>/dev/null | tr '\n' ' '
    echo
done
ls /sys/devices/system/cpu/cpu0/cpuidle 2>/dev/null && for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do
    echo "$s $(cat $s/name) usage=$(cat $s/usage) time=$(cat $s/time)"
done

sec "питание"
for p in /sys/class/power_supply/*; do
    echo "-- $p"
    cat "$p/uevent" 2>/dev/null
done
cat /sys/power/state /sys/power/wake_lock /sys/power/autosleep 2>/dev/null

sec "папки прошивки"
for d in /ebrmain /ebrmain/bin /ebrmain/lib /ebrmain/config /ebrmain/scripts /ebrmain/fonts \
    /mnt/secure /mnt/secure/runonce /mnt/secure/etc /mnt/ext1/system /mnt/ext1/system/bin \
    /mnt/ext1/system/config /mnt/ext1/system/fonts /mnt/ext1/system/state /etc /etc/init.d /var/run /var/run/task; do
    echo "-- $d"
    ls -la "$d" 2>&1 | head -200
done
ls -la /usr/bin/sudo /bin/su /usr/bin/su 2>&1

sec "монтирование"
cat /proc/mounts
df 2>/dev/null

sec "init"
cat /etc/inittab 2>&1
for f in /etc/init.d/* /etc/rc.local /ebrmain/scripts/*; do
    [ -f "$f" ] || continue
    s=$(wc -c < "$f")
    echo "$f: $s байт"
    [ "$s" -lt 200000 ] && cp "$f" "$OUT/etc/$(echo "$f" | tr '/' '_')"
done

sec "библиотеки и программы для разбора на ПК"
for f in /ebrmain/bin/bookshelf.app /ebrmain/bin/monitor.app /ebrmain/pocketbook /ebrmain/bin/pocketbook /ebrmain/bin/taskmgr.app \
    /ebrmain/bin/scanner.app /ebrmain/bin/netagent /ebrmain/lib/libinkview.so /usr/lib/libinkview.so \
    /ebrmain/config/settings/settings.json; do
    [ -e "$f" ] || { echo "$f: нет"; continue; }
    f=$(readlink -f "$f")
    s=$(wc -c < "$f")
    echo "$f: $s байт"
    [ "$s" -lt 40000000 ] && cp "$f" "$OUT/fw/"
done
ls /ebrmain/lib/libinkview* /usr/lib/libinkview* 2>/dev/null

sec "шрифты"
du -s /ebrmain/fonts /mnt/ext1/system/fonts 2>/dev/null
ls -la /ebrmain/fonts /mnt/ext1/system/fonts 2>&1 | head -150

sec "ждём 120 с, книга не трогается"
sleep 120
head -1 /proc/stat > "$OUT/stat2.txt"
procs > "$OUT/procs2.txt"
sec "процессор за 120 с: /proc/stat (user nice system idle iowait irq softirq)"
cat "$OUT/stat1.txt" "$OUT/stat2.txt"
sec "процессы (2)"
cat "$OUT/procs2.txt"
sec "конец"
date
