"""Concurrent traffic against a WordPress site: what several PHP workers see at once.

    python3 load.py <base url> <user> <application password> [clients] [ops per client]

Each client, in its own thread, mixes writes through the REST API - new posts,
edits of its own posts, comments - with page and API reads. Prints status
counts and latency per kind; exits 1 if any write did not succeed.
"""
import base64, json, sys, threading, time, urllib.request, urllib.error, random

base, user, pw = sys.argv[1], sys.argv[2], sys.argv[3]
clients = int(sys.argv[4]) if len(sys.argv) > 4 else 8
ops = int(sys.argv[5]) if len(sys.argv) > 5 else 40
auth = "Basic " + base64.b64encode(f"{user}:{pw}".encode()).decode()
lock = threading.Lock()
stats = {}          # kind -> [count by status, latencies]

def call(kind, method, route, body=None, query=""):
    url = f"{base}/?rest_route={route}{query}" if route else base + query
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json", "Authorization": auth})
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            status, payload = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, payload = e.code, e.read()
    except Exception as e:
        status, payload = f"error:{type(e).__name__}", b""
    dt = time.time() - t0
    with lock:
        s = stats.setdefault(kind, [{}, []])
        s[0][status] = s[0].get(status, 0) + 1
        s[1].append(dt)
    try:
        return status, json.loads(payload)
    except Exception:
        return status, None

def client(n):
    rnd = random.Random(n)
    mine = []
    for i in range(ops):
        r = rnd.random()
        if r < 0.30 or not mine:
            st, js = call("post", "POST", "/wp/v2/posts",
                          {"title": f"client {n} post {i}", "content": f"Written by client {n}, op {i}.", "status": "publish"})
            if st == 201 and js: mine.append(js["id"])
        elif r < 0.45:
            pid = rnd.choice(mine)
            call("edit", "POST", f"/wp/v2/posts/{pid}", {"title": f"client {n} post {pid} edited at op {i}"})
        elif r < 0.65:
            call("comment", "POST", "/wp/v2/comments", {"post": rnd.choice(mine), "content": f"comment from client {n} at op {i}"})
        elif r < 0.85:
            call("read-api", "GET", "/wp/v2/posts", query="&per_page=10")
        else:
            call("read-page", "GET", None, query=f"/?p={rnd.choice(mine)}")

t0 = time.time()
threads = [threading.Thread(target=client, args=(n,)) for n in range(clients)]
for t in threads: t.start()
for t in threads: t.join()
elapsed = time.time() - t0

bad = 0
for kind, (codes, lat) in sorted(stats.items()):
    lat.sort()
    p50, p95 = lat[len(lat) // 2], lat[int(len(lat) * 0.95)]
    print(f"{kind:10} {sum(codes.values()):4}  {codes}  p50 {p50*1000:.0f} ms  p95 {p95*1000:.0f} ms  max {lat[-1]*1000:.0f} ms")
    if kind in ("post", "edit", "comment"):
        ok = 201 if kind in ("post", "comment") else 200
        bad += sum(v for k, v in codes.items() if k != ok)
print(f"{clients} clients x {ops} ops in {elapsed:.1f}s; failed writes: {bad}")
sys.exit(1 if bad else 0)
