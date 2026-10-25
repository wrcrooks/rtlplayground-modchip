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
process.exit(failures ? 1 : 0);
