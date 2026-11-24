// The Features page's script in Node with a small DOM: every control's
// command, against the fields features.json and dot1x.json have.
//
//   node check_extras.js web/extras.html
"use strict";
const fs = require("fs");
const vm = require("vm");

const page = fs.readFileSync(process.argv[2], "utf8");
const script = [...page.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]).pop();
let failures = 0;
function check(cond, what) {
  console.log((cond ? "PASS  " : "FAIL  ") + what);
  failures += !cond;
}

class El {
  constructor(tag) {
    this.tag = tag; this.kids = []; this.dataset = {}; this.on = {}; this.value = ""; this.checked = false; this.hidden = false;
    this.disabled = false; this.files = []; this.text = ""; this.placeholder = "";
  }
  set textContent(t) { this.text = String(t); this.kids = []; }
  get textContent() { return this.text + this.kids.map((k) => k.textContent).join(""); }
  set innerHTML(v) { throw new Error("innerHTML"); }
  appendChild(k) { this.kids.push(k); return k; }
  insertRow() { return this.appendChild(new El("tr")); }
  insertCell() { return this.appendChild(new El("td")); }
  addEventListener(ev, fn) { this.on[ev] = fn; }
  fire(ev) { return this.on[ev].call(this, {}); }
  all(tag) { return this.kids.flatMap((k) => (k.tag === tag ? [k] : []).concat(k.all(tag))); }
}
const byId = {}, buttons = [];
for (const m of page.matchAll(/<(\w+)([^>]*)>/g)) {
  const e = new El(m[1]), id = /\bid="([^"]+)"/.exec(m[2]), run = /data-run="([^"]+)"/.exec(m[2]), val = /\bvalue="([^"]*)"/.exec(m[2]);
  if (val) e.value = val[1];
  if (run) { e.dataset.run = run[1]; buttons.push(e); }
  if (id) { if (byId[id[1]]) check(false, "two elements with the id " + id[1]); byId[id[1]] = e; }
}
byId["sn-alg"].value = "sha256";

const features = {
  reboot_left: 0, rollback_left: 300, uptime: 1000,
  guard: { recovery: 300, causes: { bpduguard: 1, "l2-limit": 0, "link-flap": 1 }, snoop: 1, ra: 0, ports: [
    { port: 1, disabled: "bpduguard", left: 20, trust: 1, dscp: 0, prio: -1 }, { port: 2, disabled: "", left: 0, trust: 0, dscp: 1, prio: 5 }] },
  mcast: { on: 1, querier: 0, interval: 125, restrict: 1, routers: [2], vlans: [], total: 3, groups: [{ vlan: 1, group: "239.1.1.1", ports: [1, 2] }] },
  update: { version: "v1", board: "b <i>", slot: "A", trial: 1, key: 1, size: 1, seq: 5 },
  services: { ip6: { on: 1, text: "IPv6 on\n" }, snmp: { on: 1, text: "SNMPv2c on\n" }, mqtt: { on: 0, text: "MQTT off, every 30 s\n" },
    prom: { on: 0, text: "off" }, ssh: { on: 1, text: "SSH on" } },
};
const dot1x = { radsec: 1, radius_vlan: 1, ports: [{ port: 1, guest: 30, authfail: 0, vlan: 30 }, { port: 2, guest: 0, authfail: 0, vlan: 0 }] };
const events = [{ seq: 1, t: 900, type: "link", port: 2, text: "<b>up</b>" }];
let sent = [], answer = { ok: true, body: "" }, sends = [];
const ctx = {
  console, Promise, JSON, Number, Math, Date, Object, String, Error, TypeError, setTimeout() {}, setInterval() {}, confirm: () => true,
  location: { href: "", reload() {} },
  document: { getElementById: (id) => byId[id], createElement: (t) => new El(t), querySelectorAll: () => buttons, hidden: false, activeElement: null,
    querySelector: () => buttons.find((b) => b.dataset.run === "update") },
  fetch: async (path, o) => {
    const json = { "features.json": features, "dot1x.json": dot1x, "events.json": events }[path];
    if (path === "cmd") { sent.push(o.body); return { status: 200, ok: answer.ok, text: async () => answer.body }; }
    return { status: 200, ok: true, text: async () => JSON.stringify(json) };
  },
  FormData: class { append(n, f) { this.f = f; } },
  XMLHttpRequest: class { constructor() { this.upload = {}; } open(m, p) { this.p = p; }
    send(b) { sends.push([this.p, b]); this.status = 200; this.responseText = "OK: fine\n"; this.onload(); } },
};
vm.createContext(ctx);
vm.runInContext(script, ctx);
const settle = () => new Promise((r) => setImmediate(() => setImmediate(() => setImmediate(() => setImmediate(r)))));
async function click(run, fields, want) {
  sent = [];
  for (const k in fields) byId[k].value = fields[k];
  buttons.find((b) => b.dataset.run === run).fire("click");
  for (let i = 0; i < 5; i++) await settle();
  check(sent.join("|") === want, run + ": " + (want || "refused") + (sent.join("|") === want ? "" : " but " + sent.join("|")));
}
(async () => {
  for (let i = 0; i < 5; i++) await settle();
  check(byId["ed-bpduguard"].checked && !byId["ed-l2-limit"].checked && byId["ds-on"].checked && byId["mc-on"].checked
    && byId["mc-unk"].value === "router" && byId["ed-rec"].value == 300 && byId["mq-int"].value === "30", "the settings as the switch has them");
  check(byId["ssh-on"].checked && !byId["mq-on"].checked && byId["sn-v2c"].checked, "the services too");
  check(/v1, b <i>\nSlot A, on trial/.test(byId["up-state"].textContent) && !byId["up-keep"].hidden, "the firmware, on trial");
  check(/5 min/.test(byId["rb-state"].textContent) && !byId["rb-ok"].hidden && byId["re-off"].hidden, "the rollback pending");
  check(byId.events.textContent.includes("<b>up</b>"), "events, as text");
  check(/239.1.1.11, 2/.test(byId.groups.textContent) && /and 2 more/.test(byId.groups.textContent), "multicast groups");

  const row = byId.ports.kids;
  check(row.length === 2 && /disabled: bpduguard/.test(row[0].textContent), "the ports");
  const act = async (el, set, want) => {
    sent = []; Object.assign(el, set); el.fire(el.tag === "button" ? "click" : "change");
    for (let i = 0; i < 5; i++) await settle();
    check(sent.join("|") === want, want + (sent.join("|") === want ? "" : " but " + sent.join("|")));
  };
  await act(row[0].all("button")[0], {}, "errdisable port 1 clear");
  const [trust, dscp, router, guest, fail] = row[1].all("input");
  await act(trust, { checked: true }, "dhcp-snoop trust 2 on");
  await act(dscp, { checked: false }, "qos port 2 trust none");
  await act(row[1].all("select")[0], { value: "6" }, "qos port 2 priority 6");
  await act(router, { checked: false }, "mcsnoop router 2 off");
  await act(guest, { value: "31" }, "dot1x port 2 guest-vlan 31");
  await act(fail, { value: "" }, "dot1x port 2 auth-fail-vlan off");
  for (const [id, want] of Object.entries({ "ed-bpduguard": "errdisable cause bpduguard on", "ds-ra": "dhcp-snoop ra on", "mc-q": "mcsnoop querier on",
    "dx-vlan": "dot1x radius-vlan on", "i6-on": "ip6 on", "sn-on": "snmp on", "sn-v2c": "snmp v2c on", "mq-on": "mqtt on", "pr-on": "prometheus on",
    "ssh-on": "ssh on", "ed-l2-limit": "errdisable cause l2-limit on", "ed-link-flap": "errdisable cause link-flap on", "ds-on": "dhcp-snoop on", "mc-on": "mcsnoop on" }))
    await act(byId[id], { checked: true }, want);
  await click("recovery", { "ed-rec": "600" }, "errdisable recovery 600");
  await click("recovery", { "ed-rec": "" }, "errdisable recovery off");
  await click("rollback", {}, "rollback in 10");
  await click("rollback", { "rb-min": "0" }, "");
  await click("confirm", {}, "rollback confirm");
  await click("reboot", {}, "reboot in 10");
  await click("noreboot", {}, "reboot cancel");
  await click("keep", {}, "update keep");
  await click("radsec", { "dx-fp": "AA:BB" }, "dot1x radsec AA:BB");
  await click("radsec", { "dx-fp": "" }, "");
  await click("noradsec", {}, "dot1x radsec off");
  await click("ip6addr", { "i6-addr": "2001:db8::5" }, "ip6 addr 2001:db8::5");
  await click("ip6addr", { "i6-addr": "" }, "ip6 addr none");
  await click("ip6gw", {}, "ip6 gw auto");
  await click("snuser", { "sn-user": "bob", "sn-auth": "password1", "sn-priv": "password2" }, "snmp user bob auth sha256 password1 aes password2");
  await click("snuser", { "sn-priv": "" }, "snmp user bob auth sha256 password1");
  await click("snuser", { "sn-auth": "two words" }, "");
  await click("snuseroff", {}, "snmp user bob off");
  await click("trap", { "sn-trap": "10.0.0.9", "sn-tuser": "v2c" }, "snmp trap 10.0.0.9");
  await click("trap", { "sn-tuser": "bob" }, "snmp trap 10.0.0.9 bob");
  await click("trapoff", {}, "snmp trap 10.0.0.9 off");
  await click("mqsrv", { "mq-srv": "10.0.0.5", "mq-port": "8883" }, "mqtt server 10.0.0.5 8883");
  await click("mqlogin", { "mq-user": "u", "mq-pw": "p" }, "mqtt login u p");
  await click("mqlogin", { "mq-user": "" }, "mqtt login off");
  await click("mqtls", {}, "mqtt tls off");
  await click("mqtopic", { "mq-topic": "a/b" }, "mqtt topic a/b");
  await click("mqint", { "mq-int": "60" }, "mqtt interval 60");
  await click("prtok", { "pr-tok": "" }, "prometheus token off");
  await click("newkey", {}, "ssh newkey");
  await click("update", {}, "");
  check(!sends.length, "  no file: nothing sent");
  byId["up-file"].files = [{ name: "a.mcu" }];
  await click("update", {}, "");
  check(sends.length === 1 && sends[0][0] === "update" && sends[0][1].name === "a.mcu", "  the file itself goes to /update");
  byId["rs-file"].files = [{ name: "c.txt" }];
  await click("restore", { "rs-min": "5" }, "");
  check(sends.length === 2 && sends[1][0] === "restore?confirm=5" && sends[1][1].f.name === "c.txt", "  and a configuration to /restore");
  answer = { ok: true, body: "Error: no such thing\n" };
  await click("keep", {}, "update keep");
  check(byId.msg.className === "bad" && /no such thing/.test(byId.msg.textContent), "  an error shows");
  console.log(failures ? failures + " failed" : "all passed");
  process.exit(failures ? 1 : 0);
})();
