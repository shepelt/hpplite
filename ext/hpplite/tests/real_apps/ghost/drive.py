"""Drive Ghost's Admin API like a publisher's first day. Exits 0 when every call worked."""
import json, sys, time, urllib.request, http.cookiejar, random
BASE = "http://localhost:2368"
op = urllib.request.build_opener()
cookie = {}
fails = 0
def api(method, path, body=None, ok=(200, 201, 204)):
    global fails
    req = urllib.request.Request(BASE + "/ghost/api/admin" + path, method=method,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "Origin": BASE, "Accept-Version": "v6.0"})
    if cookie: req.add_header("Cookie", "; ".join(f"{k}={v}" for k, v in cookie.items()))
    try:
        with op.open(req, timeout=60) as r:
            for h in r.headers.get_all("Set-Cookie") or []:
                k, v = h.split(";", 1)[0].split("=", 1)
                cookie[k] = v
            txt = r.read().decode()
            try: return r.status, (json.loads(txt) if txt else {})
            except ValueError: return r.status, {}
    except urllib.error.HTTPError as e:
        txt = e.read().decode()[:300]
        if e.code not in ok:
            fails += 1
            print(f"  FAIL {method} {path}: {e.code} {txt}")
        return e.code, {}
def lexical(text):
    return json.dumps({"root": {"children": [{"children": [{"detail": 0, "format": 0, "mode": "normal", "style": "",
        "text": text, "type": "extended-text", "version": 1}], "direction": "ltr", "format": "", "indent": 0,
        "type": "paragraph", "version": 1}], "direction": "ltr", "format": "", "indent": 0, "type": "root", "version": 1}})
random.seed(7)
st, _ = api("POST", "/authentication/setup/", {"setup": [{"name": "Owner", "email": "owner@example.com",
          "password": "Tq8#vLz2!pW9mKx4rN6s", "blogTitle": "HPPLite on Ghost"}]})
print("setup", st)
st, _ = api("POST", "/session/", {"username": "owner@example.com", "password": "Tq8#vLz2!pW9mKx4rN6s"})
print("session", st)
tags = []
for i in range(10):
    st, b = api("POST", "/tags/", {"tags": [{"name": f"Topic {i}", "description": f"About topic {i}"}]})
    if b: tags.append(b["tags"][0]["id"])
posts = []
for i in range(60):
    status = "published" if i % 3 else "draft"
    body = {"posts": [{"title": f"Post {i}: on durable logs", "lexical": lexical(f"Paragraph {i}. " * 40),
            "status": status, "tags": [{"id": t} for t in random.sample(tags, 2)] if tags else [],
            "custom_excerpt": f"Excerpt {i}", "featured": i % 7 == 0}]}
    if i % 10 == 9: body["posts"][0].update({"status": "scheduled", "published_at": "2030-01-01T10:00:00.000Z"})
    st, b = api("POST", "/posts/?source=html", body)
    if b: posts.append((b["posts"][0]["id"], b["posts"][0]["updated_at"]))
print("posts", len(posts))
for pid, upd in posts[:20]:
    api("PUT", f"/posts/{pid}/", {"posts": [{"title": "Edited title", "updated_at": upd, "lexical": lexical("Edited body. " * 30)}]})
for pid, _ in posts[50:55]:
    api("DELETE", f"/posts/{pid}/")
for i in range(8):
    api("POST", "/pages/", {"pages": [{"title": f"Page {i}", "lexical": lexical(f"Page {i} body"), "status": "published"}]})
nm = 0
for i in range(150):
    st, b = api("POST", "/members/", {"members": [{"email": f"reader{i}@example.com", "name": f"Reader {i}",
                 "labels": [{"name": f"cohort-{i % 5}"}], "note": "imported"}]})
    if b: nm += 1
print("members", nm)
api("PUT", "/settings/", {"settings": [{"key": "title", "value": "HPPLite on Ghost (edited)"},
                                       {"key": "description", "value": "Every write on a chain"}]})
st, b = api("POST", "/newsletters/?opt_in_existing=true", {"newsletters": [{"name": "Weekly", "description": "Weekly digest"}]})
st, b = api("GET", "/posts/?limit=all&filter=status:published")
print("published", len(b.get("posts", [])) if b else None, "fails", fails)
sys.exit(1 if fails else 0)
