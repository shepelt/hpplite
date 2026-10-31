// Drizzle ORM on its usual driver, better-sqlite3 (#62). better-sqlite3's
// bundled SQLite cuts bound strings in sqlite3_expanded_sql(), which
// statement capture reads; row capture (the default) does not, so it serves
// this driver. It opens plain paths only: HPPLite is named from the
// environment (HPPLITE_FILES / HPPLITE_PARAMS, set by suite.sh).
const Database = require("better-sqlite3");
const { drizzle } = require("drizzle-orm/better-sqlite3");
const { sqliteTable, integer, text, blob } = require("drizzle-orm/sqlite-core");
const { eq, gt, sql, count } = require("drizzle-orm");
const { step, probe, finish, assert, EXT } = require("./node_common.js");
const DIR = process.env.HPPLITE_ORM_DIR;

new Database(":memory:").loadExtension(EXT);          // once per process: later opens get HPPLite
const raw = new Database(`${DIR}/better_sqlite3.db`);  // a plain path, named by HPPLITE_FILES
raw.pragma("journal_mode = WAL");
raw.pragma("foreign_keys = ON");
const db = drizzle(raw);

const users = sqliteTable("users", {
    id: integer("id").primaryKey({ autoIncrement: true }),
    email: text("email").notNull().unique(),
    bio: text("bio"),
    avatar: blob("avatar", { mode: "buffer" }),
    createdAt: integer("created_at", { mode: "timestamp" }).notNull(),
});
const posts = sqliteTable("posts", {
    id: integer("id").primaryKey({ autoIncrement: true }),
    authorId: integer("author_id").notNull().references(() => users.id, { onDelete: "cascade" }),
    body: text("body", { mode: "json" }),
    likes: integer("likes").notNull().default(0),
});

(async () => {
    console.log("== better-sqlite3 + Drizzle, row capture");
    await step("HPPLite is on this connection, capturing rows", () => {
        const st = raw.prepare("SELECT height FROM hpplite_status").get();
        assert(st && st.height >= 1, "no hpplite_status");
    });
    await step("schema", () => {
        raw.exec(`CREATE TABLE users(id INTEGER PRIMARY KEY AUTOINCREMENT, email TEXT NOT NULL UNIQUE, bio TEXT,
                    avatar BLOB, created_at INTEGER NOT NULL);
                  CREATE TABLE posts(id INTEGER PRIMARY KEY AUTOINCREMENT,
                    author_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE, body TEXT, likes INTEGER NOT NULL DEFAULT 0)`);
    });
    // values far past the 32 bytes the bundled SQLite's expansion keeps
    const longBio = "a bio long enough to be cut at thirty-two bytes ".repeat(40);
    await step("inserts with long bound strings, blobs, JSON, dates", async () => {
        for (let i = 0; i < 20; i++) {
            const [u] = await db.insert(users).values({ email: `u${i}@example.com`, bio: longBio + i,
                avatar: Buffer.alloc(500, i), createdAt: new Date(1767225600000 + i * 1000) }).returning();
            await db.insert(posts).values({ authorId: u.id, body: { title: `post ${i}`, tags: ["a", "b", String(i)],
                text: longBio }, likes: i });
        }
        const [{ n }] = await db.select({ n: count() }).from(users);
        assert(n === 20, `users: ${n}`);
    });
    await step("updates, a transaction, deletes with a cascade", async () => {
        await db.update(posts).set({ likes: sql`${posts.likes} + 1` }).where(gt(posts.likes, 10));
        db.transaction((tx) => {
            tx.update(users).set({ bio: longBio.toUpperCase() }).where(eq(users.id, 3)).run();
            tx.insert(users).values({ email: "late@example.com", createdAt: new Date(1767225600000) }).run();
        });
        await db.delete(users).where(eq(users.id, 5));          // its post goes with it
        const [{ n }] = await db.select({ n: count() }).from(posts);
        assert(n === 19, `posts after the cascade: ${n}`);
    });
    await probe("random() and the clock in a write (statement capture refused these)", () => {
        raw.prepare("UPDATE posts SET likes = abs(random()) % 100, body = json_set(body, '$.at', datetime('now')) WHERE id < 4").run();
    });
    await step("a write logged as text (a schema change) without bound values runs", () => {
        raw.exec("CREATE INDEX posts_author ON posts(author_id)");
    });
    await step("flush", () => {
        raw.prepare("SELECT hpplite_flush()").get();
        assert(raw.prepare("SELECT pending FROM hpplite_status").get().pending === 0, "pending after flush");
    });
    raw.close();
    finish("better_sqlite3");
})();
