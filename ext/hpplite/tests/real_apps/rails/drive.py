"""16 clients at once against the Rails app: products, orders (each enqueues a
Solid Queue job), edits, deletes, reads. Prints per-kind latency; exit 1 on a failed write."""
import json, random, sys, threading, time, urllib.request
base, clients, ops = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
lock = threading.Lock(); stats = {}; fails = []
def call(kind, method, path, body=None):
    req = urllib.request.Request(base + path, method=method, data=None if body is None else json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", "Accept": "application/json"})
    t = time.time()
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            code, data = r.status, r.read()
    except urllib.error.HTTPError as e:
        code, data = e.code, e.read()
    except Exception as e:
        code, data = 0, str(e).encode()
    with lock:
        stats.setdefault(kind, []).append((time.time() - t) * 1000)
        if code >= 300 or code == 0: fails.append((kind, code, data[:120]))
    try: return json.loads(data or b"{}")
    except ValueError: return {}
seed = [call("product", "POST", "/products.json", {"product": {"name": f"seed {i}", "price": "9.99", "stock": 1000}}) for i in range(10)]
ids = [p.get("id") for p in seed if p.get("id")]
if not ids: print("seeding failed:", fails[:3]); sys.exit(1)
def client(n):
    rnd = random.Random(n)
    mine = []
    for i in range(ops):
        k = rnd.random()
        if k < 0.35:
            o = call("order", "POST", "/orders.json", {"order": {"product_id": rnd.choice(ids), "qty": rnd.randint(1, 3), "status": "new"}})
            if o.get("id"): mine.append(o["id"])
        elif k < 0.50:
            p = call("product", "POST", "/products.json", {"product": {"name": f"c{n}-{i}", "price": str(rnd.randint(1, 99)), "stock": 50}})
        elif k < 0.65:
            call("edit", "PATCH", f"/products/{rnd.choice(ids)}.json", {"product": {"price": str(rnd.randint(1, 99))}})
        elif k < 0.72 and mine:
            call("delete", "DELETE", f"/orders/{mine.pop(0)}.json")
        else:
            call("read", "GET", rnd.choice(["/products.json", "/orders.json", f"/products/{rnd.choice(ids)}.json"]))
t0 = time.time()
th = [threading.Thread(target=client, args=(n,)) for n in range(clients)]
[t.start() for t in th]; [t.join() for t in th]
for k, v in sorted(stats.items()):
    v.sort(); print(f"{k:8} {len(v):4}  p50 {v[len(v)//2]:6.0f} ms  p95 {v[int(len(v)*0.95)]:6.0f} ms")
print(f"{clients} clients x {ops} ops in {time.time()-t0:.1f}s; failed: {len(fails)}")
for f in fails[:5]: print("  ", f)
sys.exit(1 if fails else 0)
