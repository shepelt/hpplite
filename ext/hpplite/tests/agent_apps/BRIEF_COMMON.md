# Common brief (all apps)

You are building a small but real web backend that a product team would ship as a first version.
Its persistence layer is **SQLite through HPPLite**, a SQLite extension that records every write
to a blockchain so the database can be rebuilt and verified from the chain alone.

HPPLite documentation: ext/hpplite/README.md (in this repository) - treat it as the product's docs.
Do not read HPPLite's source code or its tests directory; build from the README like any user would.
The loadable module is at the path in $HPPLITE_EXT (a .dylib; load it the way the README says).

Environment your app receives (use these; do not hard-code):
- APP_PORT: port to listen on
- APP_DB: path of the SQLite database file
- HPPLITE_EXT: the HPPLite module
- HPPLITE_URI_PARAMS: the HPPLite settings as URI parameters, without hpplite=on
  (rpc=...&factory=...&chainid=...&privkey=...&datadir=...&interval=...). If your driver opens
  plain paths only, the README explains HPPLITE_FILES / HPPLITE_PARAMS.

Deliverables in your working directory:
1. The app (your choice of structure), with its dependencies installed locally in that directory
   (a venv in ./venv or ./node_modules). Network access for pip/npm is available.
2. `run.sh`: starts the server in the foreground using the environment above.
3. `scenario.sh`: drives the app end to end like a real user session, over HTTP only (curl or a
   small script), and exits 0 only if every check passed. It must:
   - start the server (via run.sh, in the background), run phase 1 of the session, stop the server;
   - apply the v2 schema change described in your app brief as a real migration on the existing
     database (with data in it), restart the server, run phase 2 (which uses the v2 feature and
     re-checks phase 1 data), stop the server.
   - Do not delete APP_DB inside scenario.sh; the caller provides a fresh path.
4. `NOTES.md`: short and factual - stack and versions, how you connected to HPPLite, every HPPLite
   error or refusal you hit (exact message), what you changed because of it, and anything in the
   README that was unclear or wrong.

Work only inside your working directory. Run scenario.sh yourself until it passes, using a fresh
APP_DB each time. Report back with: the result of your last scenario run, and the NOTES.md contents.
