// The dashboard's script in Node, against what metrics.c produced
// (test_metrics): parses history.bin, ingests two polls of metrics.json and
// checks the series the charts draw from. The DOM is a stub that accepts
// anything, so this covers the data handling, not the drawing.
//
//   node check_dashboard.js web/dashboard.html build/history.bin build/metrics.json
"use strict";
const fs = require("fs");
const vm = require("vm");

const [html, histPath, jsonPath] = process.argv.slice(2);
const page = fs.readFileSync(html, "utf8");
const scripts = [...page.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
let failures = 0;
function check(cond, what) {
  console.log((cond ? "PASS  " : "FAIL  ") + what);
  failures += !cond;
}

// Anything goes: every property is another stub, every call returns one
function stub() {
  const f = function () { return stub(); };
  return new Proxy(f, {
    get(t, k) {
      if (k === Symbol.toPrimitive) return () => 0;
      if (k === "length") return 0;
      if (!(k in t)) t[k] = stub();
      return t[k];
    },
    set(t, k, v) { t[k] = v; return true; },
  });
}
const ctx = {
  console, Math, Date, JSON, Number, String, Array, Object, DataView, ArrayBuffer, Uint8Array, isFinite, setTimeout() {},
  document: stub(), window: stub(), localStorage: stub(), location: stub(), innerWidth: 1200,
  fetch: () => new Promise(() => {}), requestAnimationFrame() {}, getComputedStyle: () => ({ getPropertyValue: () => "#123456" }),
  IntersectionObserver: undefined,
};
ctx.document.querySelectorAll = () => [];
ctx.window.IntersectionObserver = undefined;
ctx.window.devicePixelRatio = 1;
vm.createContext(ctx);
try {
  scripts.forEach((s) => vm.runInContext(s, ctx));
  check(true, "the page's scripts run");
} catch (e) {
  check(false, "the page's scripts run: " + e.stack);
  process.exit(1);
}

const hist = fs.readFileSync(histPath);
ctx.loadHistory(hist.buffer.slice(hist.byteOffset, hist.byteOffset + hist.length));
const S = vm.runInContext("S", ctx);
check(S.rx1 && S.rx1.length === 360, "history: 360 samples for port 1");
const near = (a, b) => Math.abs(a - b) <= Math.abs(b) * 0.001 + 1;
check(near(S.rx1[359][1], 100e6) && near(S.tx3[359][1], 150e6), "history: port rates in bits/s");
check(near(S.err2[359][1], 3) && near(S.dis2[359][1], 1), "history: errors and discards per second");
check(near(S.t_sw[359][1], 45.5) && near(S.m_load[359][1], 12.5) && near(S.b_load[359][1], 10), "history: temperature, loads");
check(S.rx1[359][0] - S.rx1[358][0] === 10000, "history: 10 s apart");
check(near(S.p_ib[359][1], 6) && S.p_im[359][1] === 0 && S.p_iu[359][1] > 40000, "history: packets/s by type");

// Two polls 2 s apart: rates from the counter deltas
const m1 = JSON.parse(fs.readFileSync(jsonPath, "utf8"));
const m2 = JSON.parse(JSON.stringify(m1));
m2.t += 2000;
m2.uptime += 2;
m2.ports.forEach((p, i) => {
  p.c[0] += 2 * 100e6 * (i + 1) / 8;	// in octets
  p.c[1] += 2 * 50e6 * (i + 1) / 8;	// out octets
  p.c[3] += 200;			// in multicast
});
m2.ports[1].c[10] += 6;			// 3 errors/s
m2.bridge.requests += 100;
m2.bridge.timeouts += 2;
m2.igmp.joins += 1;
m2.stp.tc += 1;
ctx.ingest(m1);
ctx.ingest(m2);
check(near(S.rx2[S.rx2.length - 1][1], 200e6) && near(S.tx2[S.tx2.length - 1][1], 100e6), "live: port rates from deltas");
check(near(S.p_im[S.p_im.length - 1][1], 600), "live: multicast packets/s, all ports");
check(near(S.e_err[S.e_err.length - 1][1], 3), "live: errors/s");
check(near(S.b_req[S.b_req.length - 1][1], 50) && near(S.b_to[S.b_to.length - 1][1], 1), "live: bridge requests and timeouts/s");
check(near(S.i_join[S.i_join.length - 1][1], 30), "live: IGMP joins per minute");
check(near(S.o_tx1[S.o_tx1.length - 1][1], -3.0) && near(S.o_rx1[S.o_rx1.length - 1][1], -6.0), "live: SFP power in dBm");
check(near(S.t_sfp1[S.t_sfp1.length - 1][1], 35.5), "live: SFP temperature");
const marks = vm.runInContext("marks", ctx);
check(marks.length === 1 && /topology/.test(marks[0].what), "live: an STP topology change marked");

// Port charts: one per top speed, slowest first, each port's state in its legend
const groups = ctx.speedGroups(m1.ports);
check(groups.length === 2 && groups[0].mbps === 2500 && groups[0].ports.map((p) => p.n).join() === "1,2,3,4"
      && groups[1].mbps === 10000 && groups[1].ports.map((p) => p.n).join() === "5,6", "ports grouped by top speed: 1-4, 5-6");
check(ctx.speedName(2500) === "2.5 Gb/s" && ctx.speedName(10000) === "10 Gb/s", "group titles");
const pc = vm.runInContext("portCharts", ctx);
check(pc[1] === pc[4] && pc[5] === pc[6] && pc[1] !== pc[5], "ports 1-4 share a chart, 5-6 another");
check(pc[4].badges[4].textContent === "down" && pc[1].badges[1].textContent === "1G", "state badges: port 4 down, port 1 at 1G");
check(pc[1].series().length === 8, "the 2.5 Gb/s chart overlays 4 ports, in and out");

// A switch reboot clears the counters: no negative rates
const m3 = JSON.parse(JSON.stringify(m2));
m3.t += 2000;
m3.ports.forEach((p) => { p.c = p.c.map(() => 0); });
const before = S.rx1.length;
ctx.ingest(m3);
check(S.rx1.length === before, "live: cleared counters give no rate");
console.log(failures ? "FAILED" : "all passed");
process.exit(failures ? 1 : 0);
