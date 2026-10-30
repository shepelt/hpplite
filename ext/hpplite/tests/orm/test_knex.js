// Knex on HPPLite through the sqlite3 npm driver, which can open URIs
// (flags OPEN_URI); better-sqlite3 cannot.
const sqlite3 = require("sqlite3");
const { uri, step, probe, finish, assert, EXT } = require("./node_common.js");

(async () => {
    console.log("== Knex (sqlite3 driver)");
    await new Promise((ok, no) => { const b = new sqlite3.Database(":memory:"); b.loadExtension(EXT, (e) => { b.close(); e ? no(e) : ok(); }); });
    const knex = require("knex")({
        client: "sqlite3", useNullAsDefault: true,
        connection: { filename: uri("knex.db"), flags: ["OPEN_URI", "OPEN_READWRITE", "OPEN_CREATE"] },
        pool: { afterCreate: (conn, done) => conn.run("PRAGMA foreign_keys = ON", done) },
    });
    await step("migrations: createTable with increments, FKs, indexes", async () => {
        await knex.schema.createTable("customers", (t) => {
            t.increments("id"); t.string("email").notNullable().unique(); t.string("name");
            t.bigInteger("created_at").notNullable();
        });
        await knex.schema.createTable("orders", (t) => {
            t.increments("id");
            t.integer("customer_id").unsigned().notNullable().references("id").inTable("customers").onDelete("CASCADE");
            t.decimal("total", 10, 2).notNullable().defaultTo(0); t.string("status").defaultTo("new").index();
            t.json("items");
        });
    });
    await step("insert returning, batchInsert", async () => {
        const ids = await knex("customers").insert([1, 2, 3].map(i => ({ email: `c${i}@x.io`, name: `C${i}`, created_at: 1767225600 + i }))).returning("id");
        assert(ids.length === 3);
        const rows = [];
        for (let i = 0; i < 300; i++) rows.push({ customer_id: 1 + (i % 3), total: i * 1.5, items: JSON.stringify([{ sku: `s${i}` }]) });
        await knex.batchInsert("orders", rows, 50);
    });
    await step("transaction with commit, rollback on throw", async () => {
        await knex.transaction(async (trx) => {
            await trx("orders").where("total", ">", 400).update({ status: "big" });
            await trx("customers").where({ id: 2 }).update({ name: knex.raw("upper(name)") });
        });
        try {
            await knex.transaction(async (trx) => { await trx("customers").insert({ email: "never@x.io", created_at: 0 }); throw new Error("app"); });
        } catch (e) { }
        assert((await knex("customers").where({ email: "never@x.io" })).length === 0, "rollback leaked");
    });
    await step("nested transaction (savepoint)", async () => {
        await knex.transaction(async (trx) => {
            await trx("orders").insert({ customer_id: 1, total: 1 });
            try { await trx.transaction(async (t2) => { await t2("orders").insert({ customer_id: 1, total: 2 }); throw new Error("inner"); }); } catch (e) { }
        });
        assert((await knex("orders").where({ total: 2 })).length === 0);
    });
    await step("upsert: onConflict().merge()", async () => {
        await knex("customers").insert({ email: "c1@x.io", name: "Again", created_at: 1 }).onConflict("email").merge(["name"]);
    });
    await step("increment, delete with cascade", async () => {
        await knex("orders").where({ customer_id: 1 }).increment("total", 10);
        await knex("customers").where({ id: 3 }).del();
        assert((await knex("orders").where({ customer_id: 3 })).length === 0, "cascade did not run");
    });
    await step("alterTable: add, rename, drop column (knex rebuilds the table)", async () => {
        await knex.schema.alterTable("orders", (t) => { t.string("note").defaultTo(""); });
        await knex.schema.alterTable("orders", (t) => { t.renameColumn("note", "memo"); });
        await knex.schema.alterTable("orders", (t) => { t.dropColumn("items"); });
    });
    await step("batchInsert of 5000 rows in one transaction (larger than a DA batch)", async () => {
        const rows = [];
        for (let i = 0; i < 5000; i++) rows.push({ customer_id: 1, total: i, status: "imported " + "z".repeat(60) });
        await knex.transaction(async (trx) => { await trx.batchInsert("orders", rows, 250); });
        assert((await knex("orders").where("status", "like", "imported%").count({ n: "*" }))[0].n === 5000, "bulk rows");
    });
    await step("t.timestamps(true, true) rows (DEFAULT CURRENT_TIMESTAMP, #56)", async () => {
        await knex.schema.createTable("audit", (t) => { t.increments("id"); t.string("what"); t.timestamps(true, true); });
        for (let i = 0; i < 20; i++) await knex("audit").insert({ what: `event ${i}` });
    });
    await step("update ... knex.fn.now()", async () => knex("orders").where({ id: 1 }).update({ status: knex.fn.now() }));
    await probe("orderByRaw('random()') read", async () => knex("orders").orderByRaw("random()").limit(3));
    await knex.destroy();
    finish("knex");
})();
