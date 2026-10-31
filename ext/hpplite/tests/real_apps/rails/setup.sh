#!/bin/sh
# A Rails 8 app on HPPLite: every production database (primary, Solid Cache,
# Solid Queue, Action Cable) captured, each on its own rollup (one key each).
#   setup.sh <dir>
# Needs Ruby >= 3.2 with the rails gem, and in the environment:
#   HPPLITE_RPC, HPPLITE_FACTORY, HPPLITE_CHAINID
#   HPPLITE_KEY_PRIMARY, HPPLITE_KEY_CACHE, HPPLITE_KEY_QUEUE, HPPLITE_KEY_CABLE
# Run with HPPLITE_EXT=<path to hpplite.dylib|so> and RAILS_ENV=production;
# HPPLITE_CAPTURE_MODE=text|rows (default rows). See README.md.
set -eu
dir=$1; here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$dir" && cd "$dir"
rails new shop --skip-git --skip-docker --skip-kamal --skip-thruster --skip-ci --skip-test \
  --skip-system-test --skip-brakeman --skip-rubocop --skip-javascript --skip-asset-pipeline > rails-new.log
cd shop
export HPPLITE_EXT=${HPPLITE_EXT:-unused-at-generate}
bin/rails generate scaffold Product name:string price:decimal stock:integer > /dev/null
bin/rails generate scaffold Order product:references qty:integer status:string > /dev/null
cp "$here/order_job.rb" app/jobs/order_job.rb
python3 - "$(pwd)" <<'PY'
import os, re, sys
a = sys.argv[1]
common = ("hpplite=on&rpc={rpc}&factory={factory}&chainid={chainid}&interval=1s"
          "&capture=<%= ENV.fetch('HPPLITE_CAPTURE_MODE', 'rows') %>").format(
    rpc=os.environ["HPPLITE_RPC"], factory=os.environ["HPPLITE_FACTORY"], chainid=os.environ["HPPLITE_CHAINID"])
def db(role, name, key, extra=""):
    return (f"  {role}:\n    <<: *default\n"
            f"    database: \"file:{a}/storage/{name}.sqlite3?{common}&privkey={os.environ[key]}&datadir={a}/storage\"\n"
            f"    extensions: [<%= ENV.fetch(\"HPPLITE_EXT\") %>]\n{extra}")
y = ("default: &default\n  adapter: sqlite3\n  max_connections: <%= ENV.fetch(\"RAILS_MAX_THREADS\") { 5 } %>\n  timeout: 5000\n\n"
     "development:\n  <<: *default\n  database: storage/development.sqlite3\n\n"
     "# HPPLite on every production database: one rollup per wallet, so one key per database\n"
     "production:\n"
     + db("primary", "production", "HPPLITE_KEY_PRIMARY")
     + db("cache", "production_cache", "HPPLITE_KEY_CACHE", "    migrations_paths: db/cache_migrate\n")
     + db("queue", "production_queue", "HPPLITE_KEY_QUEUE", "    migrations_paths: db/queue_migrate\n")
     + db("cable", "production_cable", "HPPLITE_KEY_CABLE", "    migrations_paths: db/cable_migrate\n"))
open("config/database.yml", "w").write(y)
p = "config/environments/production.rb"; s = open(p).read()
s = s.replace("config.force_ssl = true", "config.force_ssl = false").replace("config.assume_ssl = true", "config.assume_ssl = false")
open(p, "w").write(s)
p = "app/controllers/application_controller.rb"; s = open(p).read()
s = s.replace("class ApplicationController < ActionController::Base\n",
              "class ApplicationController < ActionController::Base\n  skip_forgery_protection   # the load driver posts JSON\n", 1)
open(p, "w").write(s)
p = "app/controllers/orders_controller.rb"; s = open(p).read()
s = s.replace("      if @order.save\n", "      if @order.save\n        OrderJob.perform_later(@order.id)\n", 1)
open(p, "w").write(s)
PY
echo "ready: $dir/shop"
