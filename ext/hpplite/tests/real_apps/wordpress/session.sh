#!/bin/sh
# A WordPress site's first day through WP-CLI and the web, every PHP process with HPPLite.
#   session.sh <php binary> <hpplite module> <WordPress dir> [port]   with HPPLITE_PARAMS set
# Log in <WordPress dir>/session.log; prints failures and HPPLite refusals at the end.
set -u
php=$1; mod=$2; dir=$(cd "$3" && pwd); port=${4:-8090}
here=$(cd "$(dirname "$0")" && pwd)
P="$here/php.sh $php $mod $dir"
L=$dir/session.log; : > "$L"
wp() { $P "$dir/wp-cli.phar" --path="$dir/wordpress" "$@"; }
run() { echo "\$ wp $*" >> "$L"; wp "$@" >> "$L" 2>&1 || echo "  FAIL wp $*" >> "$L"; }

run core install --url=http://localhost:$port --title="HPPLite on WordPress" --admin_user=admin \
    --admin_password='Tq8#vLz2!pW9mKx4rN6s' --admin_email=admin@example.com --skip-email
for i in 1 2 3 4 5; do run user create editor$i editor$i@example.com --role=editor --user_pass="Pw-$i-Tq8#vLz2!pW9m"; done
run post generate --count=100 --post_type=post --post_status=publish
run post generate --count=20 --post_type=page
run term create category News; run term create category Tech; run term create post_tag hpplite
ids=$(wp post list --post_type=post --format=ids --posts_per_page=40 2>/dev/null | tr ' ' '\n')
for id in $(echo "$ids" | head -30); do run post term add $id category News; run post term add $id post_tag hpplite; done
for id in $(echo "$ids" | head -20); do run post update $id --post_title="Edited $id" --post_content="Edited content for $id"; done
for id in $(echo "$ids" | sed -n 31,36p); do run post delete $id --force; done
for id in $(echo "$ids" | head -15); do run comment create --comment_post_ID=$id --comment_content="A comment on $id" --comment_author=reader; done
run option update blogdescription "Every write on a chain"
run option update posts_per_page 25
run theme activate twentytwentyfour
run plugin activate akismet
run cron event run --due-now
run post meta add 1 hpplite_note "meta value"

# the web: pages, and writes through the REST API (each request its own PHP process state)
(cd "$dir/wordpress" && exec $P -S localhost:$port) >> "$dir/server.log" 2>&1 &
srv=$!; sleep 2
u=http://localhost:$port
for p in / "/?p=1" "/?rest_route=/wp/v2/posts&per_page=5" "/?rest_route=/wp/v2/pages"; do
  echo "GET $p $(curl -s -o /dev/null -w '%{http_code}' "$u$p")" >> "$L"
done
ap=$(wp user application-password create admin session --porcelain 2>>"$L")
post() {   # path, JSON body (built with printf: macOS sh is bash, which brace-expands {...,...})
  echo "POST $1 $(curl -s -o /dev/null -w '%{http_code}' -u "admin:$ap" -H 'Content-Type: application/json' \
    -d "$2" "$u/?rest_route=$1")" >> "$L"
}
for i in 1 2 3 4 5; do
  post /wp/v2/posts "$(printf '{"title":"REST post %s","content":"Posted through the REST API %s","status":"publish"}' $i $i)"
done
post /wp/v2/comments '{"post":1,"content":"A comment through the REST API"}'
kill $srv 2>/dev/null; pkill -f "localhost:$port" 2>/dev/null; wait 2>/dev/null

echo "commands failed: $(grep -c '  FAIL wp' "$L")"
grep -E '^(GET|POST) ' "$L" | tr '\n' ';'; echo
cat "$L" "$dir/server.log" | grep -o 'statement refused.*' | sort | uniq -c | cut -c1-200
