// Drizzle ORM on HPPLite. better-sqlite3 (Drizzle's usual driver) opens plain
// paths only, never URIs, so it cannot turn HPPLite on; this uses Node's
// built-in node:sqlite through Drizzle's sqlite-proxy driver.
const { DatabaseSync } = require("node:sqlite");
const { drizzle } = require("drizzle-orm/sqlite-proxy");
const { sqliteTable, integer, text, primaryKey } = require("drizzle-orm/sqlite-core");
const { eq, gt, sql, and, inArray, count } = require("drizzle-orm");
const { uri, step, probe, finish, assert, EXT } = require("./node_common.js");

const boot = new DatabaseSync(":memory:", { allowExtension: true });
boot.loadExtension(EXT); boot.close();
const raw = new DatabaseSync(uri("drizzle.db"));
raw.exec("PRAGMA foreign_keys = ON");

const db = drizzle(async (q, params, method) => {
    const st = raw.prepare(q);
    if (method === "run") { st.run(...params); return { rows: [] }; }
    st.setReturnArrays(true);
    if (method === "get") return { rows: st.get(...params) };
    return { rows: st.all(...params) };
});

const users = sqliteTable("users", {
    id: integer("id").primaryKey({ autoIncrement: true }),
    email: text("email").notNull().unique(),
    name: text("name").notNull(),
    createdAt: integer("created_at", { mode: "timestamp" }).notNull().$defaultFn(() => new Date(1767225600000)),
});
const posts = sqliteTable("posts", {
    id: integer("id").primaryKey({ autoIncrement: true }),
    authorId: integer("author_id").notNull().references(() => users.id, { onDelete: "cascade" }),
    title: text("title").notNull(),
    body: text("body", { mode: "json" }).$type(),
    published: integer("published", { mode: "boolean" }).notNull().default(false),
    likes: integer("likes").notNull().default(0),
});
const tags = sqliteTable("tags", { id: integer("id").primaryKey(), name: text("name").notNull().unique() });
const postTags = sqliteTable("post_tags", {
    postId: integer("post_id").notNull().references(() => posts.id, { onDelete: "cascade" }),
    tagId: integer("tag_id").notNull().references(() => tags.id, { onDelete: "cascade" }),
}, (t) => [primaryKey({ columns: [t.postId, t.tagId] })]);

(async () => {
    console.log("== Drizzle (sqlite-proxy over node:sqlite)");
    await step("schema as drizzle-kit generates it", async () => {
        raw.exec(`CREATE TABLE users (id integer PRIMARY KEY AUTOINCREMENT NOT NULL, email text NOT NULL, name text NOT NULL, created_at integer NOT NULL);
CREATE UNIQUE INDEX users_email_unique ON users (email);
CREATE TABLE posts (id integer PRIMARY KEY AUTOINCREMENT NOT NULL, author_id integer NOT NULL, title text NOT NULL, body text,
  published integer DEFAULT false NOT NULL, likes integer DEFAULT 0 NOT NULL,
  FOREIGN KEY (author_id) REFERENCES users(id) ON UPDATE no action ON DELETE cascade);
CREATE TABLE tags (id integer PRIMARY KEY NOT NULL, name text NOT NULL);
CREATE UNIQUE INDEX tags_name_unique ON tags (name);
CREATE TABLE post_tags (post_id integer NOT NULL, tag_id integer NOT NULL, PRIMARY KEY(post_id, tag_id),
  FOREIGN KEY (post_id) REFERENCES posts(id) ON UPDATE no action ON DELETE cascade,
  FOREIGN KEY (tag_id) REFERENCES tags(id) ON UPDATE no action ON DELETE cascade);`);
    });
    await step("insert ... returning, multi-row values", async () => {
        const us = await db.insert(users).values([1, 2, 3, 4].map(i => ({ email: `u${i}@x.io`, name: `U${i}` }))).returning({ id: users.id });
        assert(us.length === 4);
        await db.insert(tags).values([{ id: 1, name: "news" }, { id: 2, name: "tech" }]);
        for (const u of us) {
            const ps = await db.insert(posts).values([0, 1, 2].map(k => ({ authorId: u.id, title: `post ${u.id}.${k}`, body: { k, html: "<p>hi</p>" } }))).returning();
            await db.insert(postTags).values(ps.map(p => ({ postId: p.id, tagId: 1 + (p.id % 2) })));
        }
    });
    await step("update with sql expressions, where/and/inArray", async () => {
        await db.update(posts).set({ likes: sql`${posts.likes} + 5`, published: true }).where(and(gt(posts.id, 3), inArray(posts.authorId, [1, 2])));
    });
    await step("upsert: onConflictDoUpdate", async () => {
        await db.insert(tags).values({ id: 1, name: "news" }).onConflictDoUpdate({ target: tags.id, set: { name: sql`excluded.name || '!'` } });
        await db.insert(tags).values({ id: 3, name: "misc" }).onConflictDoNothing();
    });
    await step("transaction commit and rollback", async () => {
        raw.exec("BEGIN");
        await db.insert(tags).values({ id: 10, name: "kept" });
        raw.exec("COMMIT");
        raw.exec("BEGIN");
        await db.insert(tags).values({ id: 11, name: "dropped" });
        raw.exec("ROLLBACK");
        const r = await db.select({ n: count() }).from(tags).where(eq(tags.id, 11));
        assert(r[0].n === 0, "rolled back row present");
    });
    await step("joins and aggregates (reads)", async () => {
        const r = await db.select({ name: users.name, n: count(posts.id) }).from(users).leftJoin(posts, eq(posts.authorId, users.id)).groupBy(users.id);
        assert(r.length === 4);
    });
    await step("delete with ON DELETE CASCADE", async () => {
        await db.delete(users).where(eq(users.email, "u4@x.io"));
        const r = await db.select({ n: count() }).from(posts).where(eq(posts.authorId, 4));
        assert(r[0].n === 0, "cascade did not run");
    });
    await step("drizzle-kit push style migration: add column, table rebuild", async () => {
        raw.exec(`ALTER TABLE posts ADD slug text;
PRAGMA foreign_keys=OFF;
CREATE TABLE __new_tags (id integer PRIMARY KEY NOT NULL, name text NOT NULL, color text DEFAULT 'gray' NOT NULL);
INSERT INTO __new_tags(id, name) SELECT id, name FROM tags;
DROP TABLE tags;
ALTER TABLE __new_tags RENAME TO tags;
PRAGMA foreign_keys=ON;
CREATE UNIQUE INDEX tags_name_unique ON tags (name);
UPDATE posts SET slug = lower(replace(title, ' ', '-'));`);
    });
    await step("5000-row seed in one transaction (larger than a DA batch)", async () => {
        raw.exec("BEGIN");
        for (let k = 0; k < 50; k++) {
            await db.insert(posts).values(Array.from({ length: 100 }, (_, i) => ({ authorId: 1, title: `seed ${k}.${i} ` + "w".repeat(60), body: { k, i } })));
        }
        raw.exec("COMMIT");
        const r = await db.select({ n: count() }).from(posts).where(sql`${posts.title} like 'seed %'`);
        assert(r[0].n === 5000, "seed rows");
    });
    await step("default sql`(unixepoch())` and (CURRENT_TIMESTAMP) rows (the drizzle template defaults, #56)", async () => {
        raw.exec("CREATE TABLE sessions (id text PRIMARY KEY, created_at integer DEFAULT (unixepoch()) NOT NULL)");
        raw.exec("CREATE TABLE events (id integer PRIMARY KEY, at text DEFAULT (CURRENT_TIMESTAMP), ms text DEFAULT (strftime('%Y-%m-%d %H:%M:%f', 'now')))");
        for (let i = 0; i < 20; i++) {
            raw.prepare("INSERT INTO sessions(id) VALUES(?)").run(`s${i}`);
            raw.prepare("INSERT INTO events(id) VALUES(?)").run(i);
        }
    });
    await probe("orderBy(sql`random()`) read", async () => db.select().from(posts).orderBy(sql`random()`));
    await probe("SELECT last_insert_rowid() read", async () => raw.prepare("SELECT last_insert_rowid()").get());
    raw.close();
    finish("drizzle");
})();
