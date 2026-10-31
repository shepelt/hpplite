#!/bin/sh
# A Rails 8 app on HPPLite: every production database (primary, Solid Cache,
# Solid Queue, Action Cable) captured, each on its own rollup of one wallet.
#   setup.sh <dir>
# Needs Ruby >= 3.2 with the rails gem. Run with HPPLITE_EXT, HPPLITE_FILES
# and HPPLITE_PARAMS (dbname={file}) in the environment: see README.md.
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
python3 - <<'PY'
# plain paths: HPPLITE_FILES and HPPLITE_PARAMS (dbname={file}) do the rest
y = """default: &default
  adapter: sqlite3
  max_connections: <%= ENV.fetch("RAILS_MAX_THREADS") { 5 } %>
  timeout: 5000
  extensions: [<%= ENV.fetch("HPPLITE_EXT") %>]

development:
  <<: *default
  database: storage/development.sqlite3

production:
  primary:
    <<: *default
    database: storage/production.sqlite3
  cache:
    <<: *default
    database: storage/production_cache.sqlite3
    migrations_paths: db/cache_migrate
  queue:
    <<: *default
    database: storage/production_queue.sqlite3
    migrations_paths: db/queue_migrate
  cable:
    <<: *default
    database: storage/production_cable.sqlite3
    migrations_paths: db/cable_migrate
"""
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
