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

// The Features page's commands
const f0 = inject.indexOf("var mergeFeatures=mergeConf;"), f1 = inject.indexOf("\n  };", f0);
check(f0 > 0 && f1 > f0, "the Features script wraps mergeConf");
vm.runInContext(inject.slice(f0, f1 + 5), ctx);
const keep = ["errdisable recovery 600", "errdisable recovery off", "errdisable cause link-flap on", "dhcp-snoop on", "dhcp-snoop trust 3 on",
  "dhcp-snoop ra off", "qos port 2 trust dscp", "qos port 10 priority 5", "qos port 2 priority off", "mcsnoop on", "mcsnoop querier on",
  "mcsnoop interval 60", "mcsnoop unknown router", "mcsnoop router 4 on", "dot1x radius-vlan on", "dot1x port 2 guest-vlan 30",
  "dot1x port 2 auth-fail-vlan off", "dot1x radsec AA:BB:CC", "dot1x radsec off", "snmp v2c off", "snmp user bob auth sha256 password1 aes password2",
  "snmp user bob auth sha password1", "snmp trap 10.0.0.9", "snmp trap 10.0.0.9 bob", "ip6 on", "ip6 addr 2001:db8::5", "ip6 gw auto",
  "prometheus on", "prometheus token abc", "ssh on", "mqtt on", "mqtt server 10.0.0.5 8883", "mqtt login u p", "mqtt login off",
  "mqtt topic rack/sw1", "mqtt tls off", "mqtt interval 30"];
const lost = keep.filter((l) => !ctx.isConfCmd(l));
check(!lost.length, "the Features page's settings count as config " + lost.join(", "));
const odd = ["errdisable port 2 clear", "rollback confirm", "reboot now", "update keep", "ssh newkey", "ssh", "mqtt", "qos", "mcsnoop"].filter((l) => ctx.isConfCmd(l));
check(!odd.length, "its actions don't " + odd.join(", "));
c = merge(base, keep.join("\n"));
check(merge(c, keep.join("\n")).sort().join("|") === c.slice().sort().join("|"), "the same settings again change nothing");
const last = (base2, log, want, what) => {
  const r = merge(base2, log).filter((l) => !base.includes(l));
  check(r.sort().join("|") === want.sort().join("|"), what + (r.join("|") === want.join("|") ? "" : ": " + r.join("|")));
};
last(["dot1x radius 10.0.0.1 2083", "dot1x radsec AA"], "dot1x radius-vlan on\ndot1x radsec BB",
  ["dot1x radius 10.0.0.1 2083", "dot1x radsec BB", "dot1x radius-vlan on"], "radius-vlan and radsec leave the RADIUS server's line alone");
last(["dot1x radius-vlan on"], "dot1x radius 10.0.0.2\ndot1x radius-vlan off", ["dot1x radius 10.0.0.2", "dot1x radius-vlan off"], "and the other way round");
last(["dot1x port 2 auto", "dot1x port 2 guest-vlan 30", "dot1x port 2 mab on"], "dot1x port 2 force-unauth\ndot1x port 2 guest-vlan 31\ndot1x port 2 auth-fail-vlan 32",
  ["dot1x port 2 force-unauth", "dot1x port 2 guest-vlan 31", "dot1x port 2 auth-fail-vlan 32", "dot1x port 2 mab on"], "a port's mode, VLANs and MAB are lines of their own");
last(["snmp user bob auth sha oldpassword", "snmp user alice auth sha password9", "snmp trap 10.0.0.9 bob", "snmp trap 10.0.0.8"],
  "snmp user bob off\nsnmp trap 10.0.0.9 off\nsnmp user alice auth sha256 newpassword", ["snmp user alice auth sha256 newpassword", "snmp trap 10.0.0.8"],
  "users and traps: replaced by name and address, gone when off");
last(["qos port 1 priority 5", "qos port 1 trust dscp", "qos port 10 priority 1"], "qos port 1 priority 6\nqos port 1 trust none",
  ["qos port 1 priority 6", "qos port 1 trust none", "qos port 10 priority 1"], "qos: port 1 isn't port 10");
last(["mqtt on", "mqtt server 10.0.0.5", "mqtt login a b"], "mqtt server 10.0.0.6 8883\nmqtt login off\nmqtt off",
  ["mqtt off", "mqtt server 10.0.0.6 8883", "mqtt login off"], "mqtt");
last(["dhcp-snoop on", "dhcp-snoop trust 1 on", "dhcp-snoop ra on"], "dhcp-snoop off\ndhcp-snoop trust 1 off",
  ["dhcp-snoop off", "dhcp-snoop trust 1 off", "dhcp-snoop ra on"], "dhcp-snoop");
last(["mcsnoop on", "mcsnoop querier on", "mcsnoop interval 60"], "mcsnoop interval 30\nmcsnoop off",
  ["mcsnoop off", "mcsnoop querier on", "mcsnoop interval 30"], "mcsnoop");

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
