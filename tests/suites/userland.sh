echo "==> userland environment"
rm -rf tests/tmp/userland
mkdir -p tests/tmp/userland/fs/source
mkdir -p tests/tmp/userland/fs/copy
mkdir -p tests/tmp/userland/http_root

printenv > tests/tmp/userland/env.out
grep '^NEWOS_USERLAND=1$' tests/tmp/userland/env.out || exit 1
grep '^PATH=' tests/tmp/userland/env.out || exit 1
grep '^MANPATH=' tests/tmp/userland/env.out || exit 1
which sh > tests/tmp/userland/which-sh.out || exit 1
which ls > tests/tmp/userland/which-ls.out || exit 1
which sql > tests/tmp/userland/which-sql.out || exit 1
grep '/sh$' tests/tmp/userland/which-sh.out || exit 1
grep '/ls$' tests/tmp/userland/which-ls.out || exit 1
grep '/sql$' tests/tmp/userland/which-sql.out || exit 1
which bash > tests/tmp/userland/which-bash.out
echo $? > tests/tmp/userland/which-bash.status
grep '^1$' tests/tmp/userland/which-bash.status || exit 1

echo "==> core text tools"
printf '%s\n' gamma alpha beta > tests/tmp/userland/lines.in || exit 1
sort tests/tmp/userland/lines.in > tests/tmp/userland/sort.out || exit 1
printf '%s\n' alpha beta gamma > tests/tmp/userland/sort.expected || exit 1
cmp tests/tmp/userland/sort.expected tests/tmp/userland/sort.out || exit 1
cat tests/tmp/userland/lines.in | grep beta | tr a-z A-Z > tests/tmp/userland/pipeline.out || exit 1
printf '%s\n' BETA > tests/tmp/userland/pipeline.expected || exit 1
cmp tests/tmp/userland/pipeline.expected tests/tmp/userland/pipeline.out || exit 1
wc -l tests/tmp/userland/lines.in > tests/tmp/userland/wc.out || exit 1
grep '3 tests/tmp/userland/lines.in' tests/tmp/userland/wc.out || exit 1
rg beta tests/tmp/userland/lines.in > tests/tmp/userland/rg.out || exit 1
grep '^3:beta$' tests/tmp/userland/rg.out || exit 1

echo "==> filesystem tools"
cp tests/tmp/userland/lines.in tests/tmp/userland/fs/source/file.txt || exit 1
ln tests/tmp/userland/fs/source/file.txt tests/tmp/userland/fs/source/hardlink.txt || exit 1
ln -s file.txt tests/tmp/userland/fs/source/symlink.txt || exit 1
cp tests/tmp/userland/fs/source/file.txt tests/tmp/userland/fs/copy/copied.txt || exit 1
mv tests/tmp/userland/fs/copy/copied.txt tests/tmp/userland/fs/copy/moved.txt || exit 1
test -f tests/tmp/userland/fs/copy/moved.txt || exit 1
readlink tests/tmp/userland/fs/source/symlink.txt > tests/tmp/userland/readlink.out || exit 1
printf '%s\n' file.txt > tests/tmp/userland/readlink.expected || exit 1
cmp tests/tmp/userland/readlink.expected tests/tmp/userland/readlink.out || exit 1
stat -c '%F %h' tests/tmp/userland/fs/source/file.txt > tests/tmp/userland/stat.out || exit 1
grep '^file 2$' tests/tmp/userland/stat.out || exit 1
find tests/tmp/userland/fs -name moved.txt > tests/tmp/userland/find.out || exit 1
grep 'tests/tmp/userland/fs/copy/moved\.txt' tests/tmp/userland/find.out || exit 1

echo "==> archive and compression tools"
gzip -c tests/tmp/userland/lines.in | gunzip -c > tests/tmp/userland/gzip.out || exit 1
cmp tests/tmp/userland/lines.in tests/tmp/userland/gzip.out || exit 1
tar -cf tests/tmp/userland/archive.tar -C tests/tmp/userland fs || exit 1
mkdir -p tests/tmp/userland/extract || exit 1
tar -xf tests/tmp/userland/archive.tar -C tests/tmp/userland/extract || exit 1
cmp tests/tmp/userland/fs/source/file.txt tests/tmp/userland/extract/fs/source/file.txt || exit 1

echo "==> shell behavior"
printf '%s\n' 'echo $0' 'echo $1' 'echo $#' 'echo $*' > tests/tmp/userland/child.sh || exit 1
sh tests/tmp/userland/child.sh first second > tests/tmp/userland/child.out || exit 1
grep 'tests/tmp/userland/child\.sh' tests/tmp/userland/child.out || exit 1
grep '^first$' tests/tmp/userland/child.out || exit 1
grep '^2$' tests/tmp/userland/child.out || exit 1
grep '^first second$' tests/tmp/userland/child.out || exit 1
command -v ls || exit 1
alias hi='echo alias-ok'
hi > tests/tmp/userland/alias.out || exit 1
grep '^alias-ok$' tests/tmp/userland/alias.out || exit 1

echo "==> manuals and sql"
man ls > tests/tmp/userland/man-ls.out || exit 1
grep '^LS$' tests/tmp/userland/man-ls.out || exit 1
rm -f tests/tmp/userland/userland.db
sql tests/tmp/userland/userland.db 'CREATE TABLE items(id, name); INSERT INTO items VALUES (1, alpha), (2, beta); SELECT name FROM items WHERE id = 2;' > tests/tmp/userland/sql.out || exit 1
grep '^beta$' tests/tmp/userland/sql.out || exit 1

echo "==> http tools"
printf '%s\n' hello-userland > tests/tmp/userland/http_root/index.txt || exit 1
timeout 3s $PATH/httpd -p $NEWOS_TEST_PORT -r tests/tmp/userland/http_root > tests/tmp/userland/httpd.log &
sleep 1 || exit 1
wget -q -O tests/tmp/userland/http-fetch.txt http://127.0.0.1:$NEWOS_TEST_PORT/index.txt || exit 1
grep '^hello-userland$' tests/tmp/userland/http-fetch.txt || exit 1
sleep 3 || exit 1

echo USERLAND_SUITE_OK