// What "Save to flash" keeps of the mod chip's commands: RTLPlayground's
// mergeConf() and its tables from app.js, with the patterns embed_web.py
// adds (the CONF_ lines of its index.html script) run on top.
//
//   node check_conf_merge.js rtlplayground/html/app.js build/system_https.html
"use strict";
const fs = require("fs");
const vm = require("vm");

const [appPath, injectPath] = process.argv.slice(2);
const app = fs.readFileSync(appPath, "utf8");
const inject = fs.readFileSync(injectPath, "utf8");
let failures = 0;
function check(cond, what) {
  console.log((cond ? "PASS  " : "FAIL  ") + what);
  failures += !cond;
}

// From a declaration to the closing brace at the start of a line
function piece(start) {
  const i = app.indexOf(start);
  if (i < 0) throw new Error("app.js has no " + start);
  const j = app.indexOf("\n}", i);
  return app.slice(i, j + 2);
}
const ctx = {};
vm.createContext(ctx);
vm.runInContext(piece("var CONF_CMDS=[").replace(/\];[\s\S]*/, "];"), ctx);
vm.runInContext(piece("function isConfCmd("), ctx);
vm.runInContext(piece("var CONF_OVERWRITE=[").replace(/\];[\s\S]*/, "];"), ctx);
vm.runInContext(piece("var CONF_TOGGLE=[").replace(/\];[\s\S]*/, "];"), ctx);
vm.runInContext(piece("function mergeConf("), ctx);
const adds = inject.split("\n").filter((l) => /^\s*(CONF_\w+\.push|\/\^)/.test(l)).join("\n");
check(/CONF_CMDS\.push/.test(adds), "the injected script extends CONF_CMDS");
vm.runInContext(adds, ctx);
// The 802.1X script's wrapper round mergeConf (one mode line per port)
const w0 = inject.indexOf("var merge=mergeConf;"), w1 = inject.indexOf("\n  };", w0);
check(w0 > 0 && w1 > w0, "the 802.1X script wraps mergeConf");
vm.runInContext(inject.slice(w0, w1 + 5), ctx);

const merge = (base, log) => vm.runInContext("mergeConf", ctx)(base, [log]);
const base = ["ip 192.168.10.247", "gw 192.168.10.1", "netmask 255.255.255.0"];

check(ctx.isConfCmd("https on") && ctx.isConfCmd("https off"), "https on|off count as config");
check(!ctx.isConfCmd("https newcert") && !ctx.isConfCmd("https"), "https newcert and the status query don't");
let c = merge(base, "https on");
check(c.includes("https on"), "Save keeps https on");
c = merge(base.concat("https on"), "https off");
check(c.includes("https off") && !c.includes("https on"), "https off replaces a saved https on");
c = merge(base, "https on\nhttps newcert\nhttps off\nhttps on");
check(c.filter((l) => /^https/.test(l)).join("|") === "https on", "only the last https state is kept");
check(base.every((l) => c.includes(l)), "the network lines stay");

c = merge(base.concat("snmp off", "snmp community public"), "snmp on\nsnmp community s3cret\nsnmp location rack 2");
check(c.includes("snmp on") && !c.includes("snmp off"), "snmp on replaces snmp off");
check(c.includes("snmp community s3cret") && !c.includes("snmp community public"), "a new community replaces the old");
check(c.includes("snmp location rack 2"), "a location with spaces is kept");

c = merge(base.concat("t3 on"), "t3 on 400");
check(c.filter((l) => /^t3/.test(l)).join("|") === "t3 on 400", "t3 on 400 replaces t3 on");
c = merge(base.concat("t3 on 400"), "t3 off");
check(c.filter((l) => /^t3/.test(l)).join("|") === "t3 off", "t3 off replaces t3 on 400");
check(!ctx.isConfCmd("t3 test"), "t3 test doesn't count as config");

console.log(failures ? failures + " failed" : "all passed");
// 802.1X
check(ctx.isConfCmd("dot1x on") && ctx.isConfCmd("dot1x port 2 auto") && ctx.isConfCmd("dot1x port 2 mab on")
  && ctx.isConfCmd("dot1x radius 10.1.2.3 1812") && ctx.isConfCmd("dot1x secret a b c") && ctx.isConfCmd("dot1x reauth 3600"),
  "the dot1x settings count as config");
check(!ctx.isConfCmd("dot1x") && !ctx.isConfCmd("dot1x port 2 restart"), "dot1x status and restart don't");
c = merge(base.concat("dot1x port 2 auto", "dot1x port 2 mab on", "dot1x secret old", "dot1x radius 10.0.0.1"),
  "dot1x port 2 force-unauth\ndot1x port 3 auto\ndot1x secret new one\ndot1x radius 10.0.0.2 1645\ndot1x on\ndot1x port 2 restart");
check(c.filter((l) => /^dot1x port 2 /.test(l)).sort().join("|") === "dot1x port 2 force-unauth|dot1x port 2 mab on",
  "a new mode replaces port 2's and keeps its mab line");
check(c.includes("dot1x port 3 auto") && c.includes("dot1x on"), "port 3 and dot1x on are kept");
check(c.includes("dot1x secret new one") && !c.includes("dot1x secret old"), "a new secret replaces the old");
check(c.includes("dot1x radius 10.0.0.2 1645") && !c.includes("dot1x radius 10.0.0.1"), "a new server replaces the old");
c = merge(c, "dot1x port 2 mab off\ndot1x off");
check(c.includes("dot1x port 2 mab off") && !c.includes("dot1x port 2 mab on") && c.includes("dot1x off") && !c.includes("dot1x on"),
  "mab off and dot1x off replace their on lines");

// ACL (RTLPlayground's acl command; app.js's own patterns)
check(ctx.isConfCmd("acl 2 tcp dport 80 drop 3") && ctx.isConfCmd("acl meter 1 1000 kbps 16000"), "acl rules and meters count as config");
check(!ctx.isConfCmd("acl show") && !ctx.isConfCmd("acl counter reset"), "acl show and counter reset don't");
c = merge(base.concat("acl 3 icmp drop 4", "acl 1 udp dport 53 drop 3", "acl 2 and dip 10.0.0.1"),
  "acl 1 tcp dport 80 drop 3\nacl show\nacl 5 ethertype 86dd drop 2");
const rules = c.filter((l) => /^acl \d/.test(l));
check(rules.join("|") === "acl 1 tcp dport 80 drop 3|acl 2 and dip 10.0.0.1|acl 3 icmp drop 4|acl 5 ethertype 86dd drop 2",
  "a new acl 1 replaces the old one, and the rules stay in rule order");
c = merge(c, "acl 1 off");
check(!c.some((l) => /^acl [12] /.test(l)) && c.includes("acl 3 icmp drop 4"), "acl 1 off removes rule 1 and the and rule joined to it");

process.exit(failures ? 1 : 0);
