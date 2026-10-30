"""Shared helpers for the ORM smoke tests (tests/orm/run_orm.sh).

Each suite drives one ORM the way an app (or a coding agent) would, against
real contracts on anvil, then hands its writer file to verify(): flush, open
a fresh replica on another file with the same wallet, and require the same
height, the same state root and the same schema and rows. Probes are things
an app plausibly does that HPPLite may refuse; a refusal is recorded, never
a failure. Divergence is always a failure.
"""
import os, sqlite3, sys, tempfile, traceback

EXT = os.environ["HPPLITE_EXT"]
RPC = os.environ["HPPLITE_ANVIL_RPC"]
FACTORY = os.environ["HPPLITE_ANVIL_FACTORY"]
KEY = os.environ["HPPLITE_ANVIL_KEY0"]
CHAINID = os.environ.get("HPPLITE_ANVIL_CHAINID", "31337")
DIR = os.environ.get("HPPLITE_ORM_DIR") or tempfile.mkdtemp(prefix="hpplite-orm-")

_c = sqlite3.connect(":memory:")
_c.enable_load_extension(True)
_c.load_extension(os.path.splitext(EXT)[0])
_c.close()

def uri(name, extra=""):
    return (f"file:{DIR}/{name}?hpplite=on&rpc={RPC}&factory={FACTORY}&chainid={CHAINID}"
            f"&privkey={KEY}&datadir={DIR}&interval=0{extra}")

failures, probes = [], []

def step(name, fn):
    """Part of the app's normal flow: must work."""
    try:
        fn()
        print(f"  ok      {name}")
    except Exception as e:
        failures.append(name)
        print(f"  FAIL    {name}: {type(e).__name__}: {e}")
        traceback.print_exc(limit=3)

def probe(name, fn):
    """Something an app may do; HPPLite may refuse it. Recorded either way."""
    try:
        fn()
        probes.append((name, "works"))
        print(f"  works   {name}")
    except Exception as e:
        msg = str(e).splitlines()[0][:120]
        probes.append((name, f"refused: {msg}"))
        print(f"  refused {name}: {msg}")

def _snapshot(c):
    objs = c.execute("SELECT type, name, sql FROM sqlite_master "
                     "WHERE name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\' AND name NOT LIKE 'sqlite\\_autoindex%' ESCAPE '\\' "
                     "ORDER BY type, name").fetchall()
    data = {}
    for t, n, _ in objs:
        if t != "table":
            continue
        ncol = len(c.execute(f'PRAGMA table_info("{n}")').fetchall())
        order = ", ".join(str(i + 1) for i in range(ncol))
        data[n] = c.execute(f'SELECT * FROM "{n}" ORDER BY {order}').fetchall()
    return objs, data

STATUS = "height, pending, state_root, log_errors, replay_skipped, roots_compared, root_mismatches, foreign_batches"

def _status(c):
    return dict(zip([s.strip() for s in STATUS.split(",")], c.execute(f"SELECT {STATUS} FROM hpplite_status").fetchone()))

def verify(writer_file):
    """The writer's file against a replica rebuilt from the chain alone."""
    print("  -- replica check")
    w = sqlite3.connect(uri(writer_file), uri=True)
    w.execute("SELECT hpplite_flush()").fetchone()
    sw = _status(w)
    objs_w, data_w = _snapshot(w)
    w.close()
    r = sqlite3.connect(uri("replica_" + writer_file), uri=True)
    sr = _status(r)
    objs_r, data_r = _snapshot(r)
    r.close()
    rows = sum(len(v) for v in data_w.values())
    print(f"  writer  height {sw['height']} pending {sw['pending']} log_errors {sw['log_errors']} "
          f"tables {len(data_w)} rows {rows}")
    print(f"  replica height {sr['height']} roots_compared {sr['roots_compared']} root_mismatches {sr['root_mismatches']} "
          f"replay_skipped {sr['replay_skipped']} foreign {sr['foreign_batches']}")
    checks = [
        ("writer posted everything", sw["pending"] == 0),
        ("writer log errors", sw["log_errors"] == 0),
        ("replica height", sr["height"] == sw["height"]),
        ("replica state root", sr["state_root"] == sw["state_root"]),
        ("replica root mismatches", sr["root_mismatches"] == 0),
        ("replica replay skipped", sr["replay_skipped"] == 0),
        ("replica schema", objs_r == objs_w),
        ("replica rows", data_r == data_w),
    ]
    for name, ok in checks:
        if not ok:
            failures.append(name)
            print(f"  FAIL    {name}")
    if objs_r != objs_w or data_r != data_w:
        for n in sorted(set(data_w) | set(data_r)):
            if data_w.get(n) != data_r.get(n):
                print(f"          table {n}: writer {len(data_w.get(n, []))} rows, replica {len(data_r.get(n, []))}")
    if all(ok for _, ok in checks):
        print(f"  ok      replica matches the writer (root {sw['state_root'][:18]}...)")

def finish(suite):
    print(f"== {suite}: {len(failures)} failure(s); probes:")
    for n, o in probes:
        print(f"     {n}: {o}")
    sys.exit(1 if failures else 0)

if __name__ == "__main__":       # orm_common.py <writer file>: the replica check for the Node suites
    verify(sys.argv[1])
    finish(sys.argv[1])
