// HPPLite turned on from the environment (#55): HPPLITE_FILES names the
// file, HPPLITE_PARAMS carries the settings (both set by suite.sh), and the
// application opens a plain path - no URI - as drivers that cannot open
// URIs do.
//  - node:sqlite, plain path, Drizzle on top: must work; the replica check
//    afterwards compares it with the chain.
const { DatabaseSync } = require("node:sqlite");
const { drizzle } = require("drizzle-orm/sqlite-proxy");
const { sqliteTable, integer, text } = require("drizzle-orm/sqlite-core");
const { sql, count } = require("drizzle-orm");
const { step, finish, assert, EXT } = require("./node_common.js");
const DIR = process.env.HPPLITE_ORM_DIR;

(async () => {
    console.log("== plain-path opens, enabled from the environment");
    const boot = new DatabaseSync(":memory:", { allowExtension: true });
    boot.loadExtension(EXT); boot.close();
    const raw = new DatabaseSync(`${DIR}/env_open.db`);           // a plain path
    const db = drizzle(async (q, params, method) => {
        const st = raw.prepare(q);
        if (method === "run") { st.run(...params); return { rows: [] }; }
        st.setReturnArrays(true);
        return { rows: method === "get" ? st.get(...params) : st.all(...params) };
    });
    const notes = sqliteTable("notes", { id: integer("id").primaryKey(), body: text("body").notNull(),
                                         at: text("at").default(sql`(CURRENT_TIMESTAMP)`) });
    await step("node:sqlite on a plain path is an HPPLite connection", async () => {
        const st = raw.prepare("SELECT height FROM hpplite_status").get();
        assert(st && st.height >= 1, "no hpplite_status on the plain-path open");
    });
    await step("Drizzle writes: long strings, timestamps, a large transaction", async () => {
        raw.exec("CREATE TABLE notes (id integer PRIMARY KEY, body text NOT NULL, at text DEFAULT (CURRENT_TIMESTAMP))");
        raw.exec("BEGIN");
        for (let k = 0; k < 20; k++)
            await db.insert(notes).values(Array.from({ length: 100 }, (_, i) => ({ id: k * 100 + i, body: `note ${k}.${i} ` + "long text ".repeat(8) })));
        raw.exec("COMMIT");
        const r = await db.select({ n: count() }).from(notes);
        assert(r[0].n === 2000, "rows");
    });
    raw.close();
    finish("env_open");
})();
