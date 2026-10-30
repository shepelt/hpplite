// Shared helpers for the Node ORM suites; same contract as orm_common.py.
// The replica check runs afterwards in Python (suite.sh -> orm_common.py).
const E = process.env;
const DIR = E.HPPLITE_ORM_DIR;
const uri = (name) => `file:${DIR}/${name}?hpplite=on&rpc=${E.HPPLITE_ANVIL_RPC}&factory=${E.HPPLITE_ANVIL_FACTORY}` +
    `&chainid=31337&privkey=${E.HPPLITE_ANVIL_KEY0}&datadir=${DIR}&interval=0`;
const failures = [], probes = [];
async function step(name, fn) {
    try { await fn(); console.log(`  ok      ${name}`); }
    catch (e) { failures.push(name); console.log(`  FAIL    ${name}: ${e && e.stack || e}`); }
}
async function probe(name, fn) {
    try { await fn(); probes.push([name, "works"]); console.log(`  works   ${name}`); }
    catch (e) { const m = String(e && e.message || e).split("\n")[0].slice(0, 120); probes.push([name, `refused: ${m}`]); console.log(`  refused ${name}: ${m}`); }
}
function finish(suite) {
    console.log(`== ${suite} (writer): ${failures.length} failure(s); probes:`);
    for (const [n, o] of probes) console.log(`     ${n}: ${o}`);
    process.exit(failures.length ? 1 : 0);
}
function assert(c, m) { if (!c) throw new Error(m || "assertion failed"); }
module.exports = { uri, step, probe, finish, assert, EXT: E.HPPLITE_EXT };
