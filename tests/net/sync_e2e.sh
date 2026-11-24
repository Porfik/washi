#!/bin/bash
# Проверка синхронизации против rclone serve webdav и ftp_server.py (FTP), в контейнере:
#   scripts/dev.sh bash /work/tests/net/sync_e2e.sh webdav|ftp   (FTP_MODE=list — без MLSD)
# Нужны собранные build/pc/tests/sync_run и /work/testdata (копии его файлов кладутся на «сервер»).
set -u
KIND=${1:-webdav}
W=${W:-$(cd "$(dirname "$0")/../.." && pwd)}
RUN=$W/build/pc/tests/sync_run
T=$(mktemp -d /tmp/sync-e2e-XXXXXX)
SRV=$T/srv LOC=$T/books DATA=$T/data
mkdir -p "$SRV/Фантастика/Автор" "$SRV/Техника" "$SRV/.hidden" "$LOC" "$DATA"
cp /work/testdata/fiction-2.pdf "$SRV/Фантастика/Автор/Роман.pdf"
cp /work/testdata/datasheet-1.pdf "$SRV/Техника/Справка: старт (1).pdf"
cp /work/testdata/fiction-1.pdf "$SRV/scan-jpeg.pdf"
printf 'Привет, мир\n' > "$SRV/Фантастика/заметка.txt"
printf 'не книга\n' > "$SRV/readme.md"
printf 'скрытое' > "$SRV/.hidden/x.pdf"
PORT=18080
if [ "$KIND" = ftp ]; then
    # в образе нет FTP-сервера (rclone 1.53 не умеет serve ftp) — свой, на Python; FTP_MODE=list — без MLSD
    python3 -I "$W/tests/net/ftp_server.py" "$SRV" $PORT u p "${FTP_MODE:-mlsd}" >"$T/rclone.log" 2>&1 &
    URL=ftp://127.0.0.1:$PORT/
else
    rclone serve webdav "$SRV" --addr 127.0.0.1:$PORT --user u --pass p --dir-cache-time 0s >"$T/rclone.log" 2>&1 &
    URL=http://127.0.0.1:$PORT/
fi
RC=$!
trap 'kill $RC 2>/dev/null; rm -rf "$T"' EXIT
sleep 1.5
fails=0
step() { echo; echo "=== $*"; }
run() { timeout 120 "$RUN" "$URL" u p "$LOC" "$DATA" "$@"; }
expect() { if ! run_out=$(run) ; then echo "(код возврата $?)"; fi; echo "$run_out"; for w in "$@"; do :; done; }
check() { # check "текст" вывод
    if ! grep -q -- "$1" <<<"$2"; then echo "!!! НЕТ: $1"; fails=$((fails+1)); fi
}
step "1. Первая синхронизация"
out=$(run); echo "$out"
check "новых 3," "$out" # .txt больше не книга (как в библиотеке)
test -f "$LOC/Фантастика/Автор/Роман.pdf" || { echo "!!! нет файла"; fails=$((fails+1)); }
test -f "$LOC/Техника/Справка_ старт (1).pdf" || { echo "!!! нет справки"; fails=$((fails+1)); }
test -e "$LOC/readme.md" && { echo "!!! скачан не книжный файл"; fails=$((fails+1)); }
test -e "$LOC/.hidden" && { echo "!!! скачана скрытая папка"; fails=$((fails+1)); }
find "$LOC" -name '*.part' | grep . && { echo "!!! остался .part"; fails=$((fails+1)); }
step "2. Повторная — ничего не качает"
out=$(run); echo "$out"
check "новых 0, обновлено 0" "$out"
step "3. Файл на сервере изменился"
sleep 1.1
cp /work/testdata/fiction-1.pdf "$SRV/scan-jpeg.pdf"; echo " " >> "$SRV/scan-jpeg.pdf"
out=$(run); echo "$out"
check "новых 0, обновлено 1" "$out"
cmp "$SRV/scan-jpeg.pdf" "$LOC/scan-jpeg.pdf" || { echo "!!! scan-jpeg.pdf не обновился"; fails=$((fails+1)); }
step "4. Файл на книге изменён и на сервере изменился — не трогаем"
echo "my notes" >> "$LOC/scan-jpeg.pdf"
cp "$LOC/scan-jpeg.pdf" "$T/scan-jpeg.local"
sleep 1.1; echo "  " >> "$SRV/scan-jpeg.pdf"
out=$(run); echo "$out"
check "конфликтов 1" "$out"
cmp "$T/scan-jpeg.local" "$LOC/scan-jpeg.pdf" || { echo "!!! изменённый на книге файл перезаписан"; fails=$((fails+1)); }
step "5. Удалили на сервере — на книге остаётся"
rm "$SRV/Техника/Справка: старт (1).pdf"
out=$(run); echo "$out"
check "на сервере удалено 1" "$out"
test -f "$LOC/Техника/Справка_ старт (1).pdf" || { echo "!!! книга удалена с книги"; fails=$((fails+1)); }
step "6. Слияние мест чтения (две копии)"
# книга: Роман на стр. 5, scan-jpeg на 3; сервер получил от «другого устройства» Роман на 40, scan-jpeg на 1
cat > "$DATA/positions.txt" <<EOP
5	100	0	-	o=0,s=0,c=0,f=0	$LOC/Фантастика/Автор/Роман.pdf
3	50	0	-	o=0,s=0,c=0,f=0	$LOC/scan-jpeg.pdf
EOP
mkdir -p "$SRV/.washi"
printf '40\t100\t0\t-\to=0,s=0,c=0,f=1\tФантастика/Автор/Роман.pdf\n1\t50\t0\t-\to=0,s=0,c=0,f=0\tuav.pdf\n' > "$SRV/.washi/positions.txt"
printf '2,7\tuav.pdf\n' > "$SRV/.washi/bookmarks.txt"
printf '1,4\t%s/scan-jpeg.pdf\n' "$LOC" > "$DATA/bookmarks.txt"
out=$(run); echo "$out"
check "мест обновлено 1" "$out"
grep -q "^40	100	0	-	o=0,s=0,c=0,f=1	$LOC/Фантастика/Автор/Роман.pdf" "$DATA/positions.txt" || { echo "!!! на книге нет 40-й"; cat "$DATA/positions.txt"; fails=$((fails+1)); }
grep -q "^3	50	.*scan-jpeg.pdf" "$DATA/positions.txt" || { echo "!!! scan-jpeg не на 3"; fails=$((fails+1)); }
echo "--- серверный positions.txt:"; cat "$SRV/.washi/positions.txt"
grep -q "^3	50	.*	scan-jpeg.pdf" "$SRV/.washi/positions.txt" || { echo "!!! на сервер не ушла стр. 3"; fails=$((fails+1)); }
echo "--- закладки на сервере:"; cat "$SRV/.washi/bookmarks.txt"
grep -q "^1,2,4,7	scan-jpeg.pdf" "$SRV/.washi/bookmarks.txt" || { echo "!!! закладки не слились"; fails=$((fails+1)); }
step "7. Повтор: ничего нового"
out=$(run); echo "$out"
check "отправить 0/0" "$out"
step "8. Отмена посреди"
rm -rf "$LOC" "$DATA/sync-books.txt"; mkdir -p "$LOC"
cp /work/testdata/datasheet-2.pdf "$SRV/Техника/big.pdf" # 44 МБ: отмена придётся на скачивание
out=$(run 120); echo "$out"
check "отменено: 1" "$out"
test -e "$LOC/Техника/big.pdf" && { echo "!!! недокачанный файл на месте"; fails=$((fails+1)); }
find "$LOC" -name '*.part' | grep . && { echo "!!! остался .part"; fails=$((fails+1)); }
step "9. Неверный пароль"
out=$(timeout 60 "$RUN" "$URL" u wrong "$LOC" "$DATA"); echo "$out"
check "ошибка: «Неверный логин" "$out"
step "10. Папки на сервере нет"
out=$(timeout 60 "$RUN" "${URL}нет-такой-папки" u p "$LOC" "$DATA"); echo "$out"
check "На сервере нет такой папки" "$out"
step "11. Сервер не отвечает"
out=$(timeout 60 "$RUN" "http://127.0.0.1:1/" u p "$LOC" "$DATA"); echo "$out"
check "в одной сети" "$out"
echo; echo "ОШИБОК: $fails"
exit $fails
