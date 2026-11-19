#!/usr/bin/env python3
"""Embed RTLPlayground's web UI and default config in the RP2350 firmware.

  embed_web.py OUT.c CONFIG_TXT HTML_DIR [HTML_DIR...]

Writes the files of each HTML_DIR gzipped (RTLPlayground's html/, then the
mod chip's own web/), with the same names and MIME types as RTLPlayground's
html_data.c, plus "/" for index.html, and config.txt as rtl_default_config.
index.html gets a "Graphs" entry in its navigation, for dashboard.html, and
an HTTPS switch in System > Services, and an 802.1X card on the MAC table tab;
the mod chip's commands (https, snmp, t3, dot1x) join the ones the web UI keeps
when it saves the config.
The gzip header carries no timestamp, so the output only changes when the
inputs do.
"""

import gzip
import pathlib
import sys

# Added to RTLPlayground's index.html, after app.js has filled the navigation
NAV_GRAPHS = """<script>
/* Added by the mod chip's build (tools/embed_web.py): its graphs page */
(function(){
  var l=document.getElementById("navlist");if(!l)return;
  var li=document.createElement("li");li.id="nv-graphs";
  li.innerHTML='<span><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke-width="2"><path d="M3 20h18M4 16l5-6 4 3 7-8"/></svg></span><span>Graphs</span>';
  li.onclick=function(){location.href="dashboard.html"};
  l.insertBefore(li,l.children[1]||null);
})();
</script>
"""

# Added to RTLPlayground's index.html, after app.js: the mod chip's commands
# for the config merge (Save keeps only lines isConfCmd() knows), and an
# HTTPS switch with the certificate fingerprint in System > Services
SYSTEM_HTTPS = """<script>
/* Added by the mod chip's build (tools/embed_web.py) */
(function(){
  CONF_CMDS.push(/^https\\s+(on|off)$/,/^snmp\\s+(on|off)$/,/^snmp\\s+(community|location|contact)\\s+\\S.*$/,
    /^t3\\s+(on(\\s+(100|400|1000))?|off)$/);
  CONF_TOGGLE.push(/^(https)\\s+(on|off)$/,/^(snmp)\\s+(on|off)$/);
  CONF_OVERWRITE.push(/^snmp\\s+(community|location|contact)\\b/,/^t3\\b/);

  var sess=document.querySelector('#tab-system [data-i18n="sy_sesstmo"]');
  if(!sess)return;
  var tr={
    en:{sy_https:"HTTPS",sy_https_fp:"Certificate SHA-256",sy_https_open:"Open over HTTPS",
      sy_https_note:"Self-signed: the first time, compare this fingerprint with the one the browser shows.",
      sy_https_off_q:"Turn HTTPS off?",sy_https_off_d:"This page is open over HTTPS and stops responding. HTTP on port 80 keeps working."},
    de:{sy_https:"HTTPS",sy_https_fp:"Zertifikat SHA-256",sy_https_open:"Über HTTPS öffnen",
      sy_https_note:"Selbst signiert: beim ersten Mal diesen Fingerabdruck mit dem im Browser angezeigten vergleichen.",
      sy_https_off_q:"HTTPS ausschalten?",sy_https_off_d:"Diese Seite ist über HTTPS geöffnet und reagiert dann nicht mehr. HTTP auf Port 80 bleibt verfügbar."},
    es:{sy_https:"HTTPS",sy_https_fp:"Certificado SHA-256",sy_https_open:"Abrir por HTTPS",
      sy_https_note:"Autofirmado: la primera vez, compare esta huella con la que muestra el navegador.",
      sy_https_off_q:"¿Desactivar HTTPS?",sy_https_off_d:"Esta página está abierta por HTTPS y dejará de responder. HTTP en el puerto 80 sigue funcionando."},
    fr:{sy_https:"HTTPS",sy_https_fp:"Certificat SHA-256",sy_https_open:"Ouvrir en HTTPS",
      sy_https_note:"Auto-signé : la première fois, comparez cette empreinte avec celle affichée par le navigateur.",
      sy_https_off_q:"Désactiver HTTPS ?",sy_https_off_d:"Cette page est ouverte en HTTPS et cessera de répondre. HTTP sur le port 80 reste disponible."},
    ja:{sy_https:"HTTPS",sy_https_fp:"証明書 SHA-256",sy_https_open:"HTTPS で開く",
      sy_https_note:"自己署名です。初回はこのフィンガープリントをブラウザーの表示と照合してください。",
      sy_https_off_q:"HTTPS をオフにしますか?",sy_https_off_d:"このページは HTTPS で開かれているため応答しなくなります。ポート 80 の HTTP は引き続き使えます。"},
    zh:{sy_https:"HTTPS",sy_https_fp:"证书 SHA-256",sy_https_open:"通过 HTTPS 打开",
      sy_https_note:"自签名：首次使用时，请将此指纹与浏览器显示的指纹核对。",
      sy_https_off_q:"关闭 HTTPS？",sy_https_off_d:"此页面通过 HTTPS 打开，关闭后将停止响应。端口 80 上的 HTTP 仍可使用。"},
  };
  for(var l in tr)if(LANG[l])for(var k in tr[l])LANG[l][k]=tr[l][k];

  var row=document.createElement("label");
  row.className="row";row.style.justifyContent="space-between";
  row.innerHTML='<span data-i18n="sy_https">HTTPS</span><span class="switch"><input type="checkbox" id="sy-https"><i></i></span>';
  var info=document.createElement("div");
  info.className="small mut";info.style.margin="-2px 0 10px";
  var at=sess.closest("label");
  at.parentNode.insertBefore(row,at);
  at.parentNode.insertBefore(info,at);
  var box=row.querySelector("input");

  function show(on,fp){
    box.checked=on;
    info.textContent="";
    if(fp&&fp!=="none"){
      info.appendChild(h("div",null,[h("span",{"data-i18n":"sy_https_fp",text:t("sy_https_fp")}),
        document.createTextNode(" "),h("span",{class:"mono",style:"word-break:break-all",text:fp})]));
      info.appendChild(h("div",{"data-i18n":"sy_https_note",text:t("sy_https_note")}));
    }
    if(on&&location.protocol!=="https:")
      info.appendChild(h("a",{href:"https://"+location.hostname+"/","data-i18n":"sy_https_open",text:t("sy_https_open")}));
  }
  function load(){
    return api("/cmd",{method:"POST",body:"https"}).then(function(r){
      var m=r.body.match(/HTTPS (on|off), certificate SHA-256 (\\S+)/);
      if(m)show(m[1]==="on",m[2]);
    }).catch(function(){});
  }
  box.addEventListener("change",function(){
    var on=box.checked;
    function go(){postCmd("https "+(on?"on":"off")).then(load,load)}
    if(!on&&location.protocol==="https:"){
      box.checked=true;
      confirmModal(t("sy_https_off_q"),t("sy_https_off_d"),go);
    }else go();
  });
  var hk=tabHooks.system,enter=hk&&hk.enter;
  if(hk)hk.enter=function(){if(enter)enter.apply(this,arguments);load()};
  i18nApply();
  if(curTab==="system")load();
})();
</script>
"""

# Added to RTLPlayground's index.html, after app.js: 802.1X (dot1x) on the MAC
# table tab, and its commands for the config merge
DOT1X = """<script>
/* Added by the mod chip's build (tools/embed_web.py): 802.1X, see src/dot1x.c */
(function(){
  if(typeof CONF_CMDS==="undefined"||!document.getElementById("tab-l2"))return;
  CONF_CMDS.push(/^dot1x\\s+(on|off)$/,/^dot1x\\s+radius\\s+(\\d{1,3}\\.){3}\\d{1,3}(\\s+\\d{1,5})?$/,/^dot1x\\s+secret\\s+\\S.*$/,
    /^dot1x\\s+reauth\\s+(\\d+|off)$/,/^dot1x\\s+port\\s+\\d+\\s+(auto|force-auth|force-unauth)$/,/^dot1x\\s+port\\s+\\d+\\s+mab\\s+(on|off)$/);
  CONF_TOGGLE.push(/^(dot1x)\\s+(on|off)$/,/^(dot1x\\s+port\\s+\\d{1,2}\\s+mab)\\s+(on|off)$/);
  CONF_OVERWRITE.push(/^dot1x\\s+radius\\b/,/^dot1x\\s+secret\\b/,/^dot1x\\s+reauth\\b/);
  /* one mode line per port, the last: an overwrite rule on "dot1x port <n>" would take its mab line too */
  var merge=mergeConf;
  mergeConf=function(base,texts){
    var c=merge(base,texts),seen={};
    for(var i=c.length-1;i>=0;i--){
      var m=c[i].match(/^dot1x port (\\d{1,2}) (auto|force-auth|force-unauth)$/);
      if(!m)continue;
      if(seen[m[1]])c.splice(i,1);else seen[m[1]]=1;
    }
    return c;
  };
  var tr={
    en:{x_title:"Port authentication (802.1X)",x_on:"802.1X",x_radius:"RADIUS server",x_secret:"Shared secret",x_unchanged:"unchanged",
      x_reauth:"Re-authenticate every (s)",x_never:"never",x_mode:"Control",x_mab:"MAB",x_state:"State",x_who:"Device",x_restart:"Restart",
      x_auto:"auto",x_fauth:"open (no 802.1X)",x_funauth:"blocked",
      x_note:"On an auto port only 802.1X (EAPOL) passes until the RADIUS server accepts the device; MAB lets in devices without a supplicant by their MAC after about 90 s. Save keeps the settings."},
    de:{x_title:"Port-Authentifizierung (802.1X)",x_radius:"RADIUS-Server",x_secret:"Gemeinsames Geheimnis",x_unchanged:"unverändert",
      x_reauth:"Neu authentifizieren alle (s)",x_never:"nie",x_mode:"Steuerung",x_state:"Status",x_who:"Gerät",x_restart:"Neu starten",
      x_fauth:"offen (kein 802.1X)",x_funauth:"gesperrt",
      x_note:"An einem Auto-Port läuft nur 802.1X (EAPOL), bis der RADIUS-Server das Gerät annimmt; MAB lässt Geräte ohne Supplicant nach etwa 90 s anhand ihrer MAC zu. Speichern behält die Einstellungen."},
    es:{x_title:"Autenticación de puertos (802.1X)",x_radius:"Servidor RADIUS",x_secret:"Secreto compartido",x_unchanged:"sin cambios",
      x_reauth:"Reautenticar cada (s)",x_never:"nunca",x_mode:"Control",x_state:"Estado",x_who:"Dispositivo",x_restart:"Reiniciar",
      x_fauth:"abierto (sin 802.1X)",x_funauth:"bloqueado",
      x_note:"En un puerto auto solo pasa 802.1X (EAPOL) hasta que el servidor RADIUS acepta el dispositivo; MAB admite por su MAC, tras unos 90 s, a los dispositivos sin suplicante. Guardar conserva los ajustes."},
    fr:{x_title:"Authentification des ports (802.1X)",x_radius:"Serveur RADIUS",x_secret:"Secret partagé",x_unchanged:"inchangé",
      x_reauth:"Réauthentifier toutes les (s)",x_never:"jamais",x_mode:"Contrôle",x_state:"État",x_who:"Appareil",x_restart:"Relancer",
      x_fauth:"ouvert (sans 802.1X)",x_funauth:"bloqué",
      x_note:"Sur un port auto, seul 802.1X (EAPOL) passe jusqu'à ce que le serveur RADIUS accepte l'appareil ; MAB admet par leur MAC, après environ 90 s, les appareils sans supplicant. Enregistrer conserve les réglages."},
    ja:{x_title:"ポート認証 (802.1X)",x_radius:"RADIUS サーバー",x_secret:"共有シークレット",x_unchanged:"変更なし",
      x_reauth:"再認証の間隔 (秒)",x_never:"しない",x_mode:"制御",x_state:"状態",x_who:"機器",x_restart:"再開",
      x_fauth:"開放 (802.1X なし)",x_funauth:"遮断",
      x_note:"auto ポートでは RADIUS サーバーが機器を受け入れるまで 802.1X (EAPOL) だけが通ります。MAB はサプリカントのない機器を約 90 秒後に MAC で受け入れます。保存すると設定が保持されます。"},
    zh:{x_title:"端口认证 (802.1X)",x_radius:"RADIUS 服务器",x_secret:"共享密钥",x_unchanged:"不变",
      x_reauth:"重新认证间隔 (秒)",x_never:"从不",x_mode:"控制",x_state:"状态",x_who:"设备",x_restart:"重新开始",
      x_fauth:"开放 (不认证)",x_funauth:"阻止",
      x_note:"在 auto 端口上,RADIUS 服务器接受设备之前只放行 802.1X (EAPOL);MAB 约 90 秒后按 MAC 放行没有请求方软件的设备。保存后设置会保留。"}
  };
  for(var l in tr)if(LANG[l])for(var k in tr[l])LANG[l][k]=tr[l][k];

  var card=document.createElement("div");card.className="card";
  card.innerHTML='<h2 data-i18n="x_title"></h2><p class="small mut" data-i18n="x_note"></p>'+
    '<div style="display:flex;gap:12px;flex-wrap:wrap;align-items:center;margin-bottom:10px">'+
    '<label class="row"><span data-i18n="x_on">802.1X</span> <span class="switch"><input type="checkbox" id="x-on"><i></i></span></label>'+
    '<label><span data-i18n="x_radius"></span> <input class="in mono" id="x-ip" size="15"> : <input class="in" id="x-port" type="number" min="1" max="65535" style="width:6em"></label>'+
    '<label><span data-i18n="x_secret"></span> <input class="in" id="x-secret" type="password" size="14"></label>'+
    '<label><span data-i18n="x_reauth"></span> <input class="in" id="x-reauth" type="number" min="60" max="86400" style="width:7em"></label>'+
    '<button class="ctl" id="x-apply" data-i18n="c_apply"></button></div>'+
    '<div class="scrollx"><table class="t" id="x-table"><thead><tr><th data-i18n="c_port"></th><th data-i18n="x_mode"></th><th data-i18n="x_mab"></th>'+
    '<th data-i18n="x_state"></th><th data-i18n="x_who"></th><th></th></tr></thead><tbody></tbody></table></div>';
  document.getElementById("tab-l2").appendChild(card);
  $("x-secret").placeholder=t("x_unchanged");$("x-reauth").placeholder=t("x_never");
  var timer=null,built=false;

  function load(){
    return getJSON("/dot1x.json").then(function(s){
      if(!built){
        $("x-on").checked=!!s.enabled;
        $("x-ip").value=s.radius;$("x-port").value=s.radius_port;$("x-reauth").value=s.reauth||"";
      }
      var tb=$("x-table").tBodies[0];
      if(!built){
        tb.innerHTML="";
        s.ports.forEach(function(p){
          var tr=tb.insertRow();tr.dataset.port=p.port;
          var sel=h("select",{class:"in"});
          [["force-auth","x_fauth"],["auto","x_auto"],["force-unauth","x_funauth"]].forEach(function(o){
            sel.appendChild(h("option",{value:o[0],text:t(o[1])}));
          });
          sel.value=p.mode;
          var mab=h("input",{type:"checkbox"});mab.checked=!!p.mab;
          sel.onchange=function(){postCmd("dot1x port "+p.port+" "+sel.value).then(load).catch(function(){})};
          mab.onchange=function(){postCmd("dot1x port "+p.port+" mab "+(mab.checked?"on":"off")).catch(function(){})};
          tr.insertCell().textContent=p.port;
          tr.insertCell().appendChild(sel);
          tr.insertCell().appendChild(mab);
          tr.insertCell();tr.insertCell().className="mono";
          tr.insertCell().appendChild(h("button",{class:"ctl",text:t("x_restart"),onclick:function(){
            postCmd("dot1x port "+p.port+" restart",true).then(load).catch(function(){})}}));
        });
        built=true;
      }
      s.ports.forEach(function(p){
        var tr=tb.querySelector('tr[data-port="'+p.port+'"]');if(!tr)return;
        tr.cells[3].innerHTML=p.mode==="force-auth"?badge(t("x_fauth")):
          badge(p.state+(p.method?" ("+p.method+")":""),p.authorized?"ok":(p.state==="held"||p.state==="blocked"?"bad":""));
        tr.cells[4].textContent=[p.user,p.mac].filter(Boolean).join("  ");
      });
    }).catch(function(){});
  }
  $("x-apply").addEventListener("click",function(){
    var c=[],ip=$("x-ip").value.trim(),port=$("x-port").value.trim(),re=$("x-reauth").value.trim(),sec=$("x-secret").value;
    if(ip)c.push("dot1x radius "+ip+(port?" "+port:""));
    if(sec)c.push("dot1x secret "+sec);
    c.push("dot1x reauth "+(re?re:"off"));
    c.push("dot1x "+($("x-on").checked?"on":"off"));
    postCmds(c).then(function(){$("x-secret").value="";built=false;load()}).catch(function(){});
  });
  var hk=tabHooks.l2,enter=hk&&hk.enter,leave=hk&&hk.leave;
  if(hk){
    hk.enter=function(){if(enter)enter.apply(this,arguments);built=false;load();clearInterval(timer);timer=setInterval(load,3000)};
    hk.leave=function(){if(leave)leave.apply(this,arguments);clearInterval(timer)};
  }
  i18nApply();
})();
</script>
"""

# Added to RTLPlayground's index.html, after app.js: the ACL rules (RTLPlayground's
# acl command, doc/acl.md) on the MAC table tab
ACL = r"""<script>
/* Added by the mod chip's build (tools/embed_web.py): ACL rules, see rtlplayground/doc/acl.md */
(function(){
  if(typeof CONF_CMDS==="undefined"||!document.getElementById("tab-l2"))return;
  var tr={
    en:{a_title:"Access control (ACL)",a_rule:"Rule",a_match:"Match, actions and ports",a_add:"Add",a_remove:"Remove",
      a_show:"Counters and meters",a_none:"No rules.",a_other:"Settings",
      a_note:"The lowest numbered matching rule decides. A rule is acl <1-64> followed by what to match, what to do and the ingress ports, e.g. tcp dport 80 drop 3. Save keeps the rules. Syntax: doc/acl.md."},
    de:{a_title:"Zugriffskontrolle (ACL)",a_rule:"Regel",a_match:"Bedingung, Aktionen und Ports",a_add:"Hinzufügen",a_remove:"Entfernen",
      a_show:"Zähler und Meter",a_none:"Keine Regeln.",a_other:"Einstellungen",
      a_note:"Die passende Regel mit der kleinsten Nummer entscheidet. Eine Regel ist acl <1-64>, dann die Bedingung, die Aktion und die Eingangsports, z. B. tcp dport 80 drop 3. Speichern behält die Regeln. Syntax: doc/acl.md."},
    es:{a_title:"Control de acceso (ACL)",a_rule:"Regla",a_match:"Condición, acciones y puertos",a_add:"Añadir",a_remove:"Quitar",
      a_show:"Contadores y medidores",a_none:"No hay reglas.",a_other:"Ajustes",
      a_note:"Decide la regla coincidente de número más bajo. Una regla es acl <1-64> seguido de la condición, la acción y los puertos de entrada, p. ej. tcp dport 80 drop 3. Guardar conserva las reglas. Sintaxis: doc/acl.md."},
    fr:{a_title:"Contrôle d'accès (ACL)",a_rule:"Règle",a_match:"Condition, actions et ports",a_add:"Ajouter",a_remove:"Supprimer",
      a_show:"Compteurs et limiteurs",a_none:"Aucune règle.",a_other:"Réglages",
      a_note:"La règle correspondante de plus petit numéro décide. Une règle s'écrit acl <1-64> suivi de la condition, de l'action et des ports d'entrée, p. ex. tcp dport 80 drop 3. Enregistrer conserve les règles. Syntaxe : doc/acl.md."},
    ja:{a_title:"アクセス制御 (ACL)",a_rule:"ルール",a_match:"条件、動作、ポート",a_add:"追加",a_remove:"削除",
      a_show:"カウンターとメーター",a_none:"ルールはありません。",a_other:"設定",
      a_note:"一致したルールのうち番号の最も小さいものが決めます。ルールは acl <1-64> の後に条件、動作、入力ポートを書きます (例: tcp dport 80 drop 3)。保存するとルールが保持されます。書式: doc/acl.md。"},
    zh:{a_title:"访问控制 (ACL)",a_rule:"规则",a_match:"匹配、动作和端口",a_add:"添加",a_remove:"删除",
      a_show:"计数器和计量器",a_none:"没有规则。",a_other:"设置",
      a_note:"匹配的规则中编号最小的决定结果。规则写作 acl <1-64>,后接匹配条件、动作和入口端口,例如 tcp dport 80 drop 3。保存后规则会保留。语法见 doc/acl.md。"}
  };
  for(var l in tr)if(LANG[l])for(var k in tr[l])LANG[l][k]=tr[l][k];

  var card=document.createElement("div");card.className="card";
  card.innerHTML='<h2 data-i18n="a_title"></h2><p class="small mut" data-i18n="a_note"></p>'+
    '<div class="scrollx"><table class="t" id="a-table"><thead><tr><th data-i18n="a_rule"></th><th data-i18n="a_match"></th><th></th></tr></thead><tbody></tbody></table></div>'+
    '<div style="display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:10px 0">'+
    '<span class="mono">acl</span><input class="in" id="a-n" type="number" min="1" max="64" style="width:5em">'+
    '<input class="in mono" id="a-rest" style="flex:1;min-width:16em" placeholder="tcp dport 80 drop 3">'+
    '<button class="ctl" id="a-add" data-i18n="a_add"></button>'+
    '<button class="ctl" id="a-show" data-i18n="a_show"></button></div>'+
    '<pre class="small mono" id="a-out" style="display:none;white-space:pre-wrap"></pre>';
  document.getElementById("tab-l2").appendChild(card);

  /* The rules as Save would write them: the saved config with the unsaved commands merged in */
  function load(){
    return Promise.all([getText("/config").catch(function(){return""}),getText("/cmd_log").catch(function(){return""})]).then(function(r){
      var cur=r[0].replace(/\0[\s\S]*$/,"").split(/\r?\n/).map(function(l){return l.trim().replace(/\s+/g," ")}).filter(Boolean);
      var lines=mergeConf(cur,[r[1].replace(/\0[\s\S]*$/,"")]).filter(function(l){return/^acl /.test(l)});
      var tb=$("a-table").tBodies[0],used={};tb.innerHTML="";$("a-out").style.display="none";
      lines.forEach(function(l){
        var m=l.match(/^acl (\d{1,2}) (.*)$/),row=tb.insertRow();
        row.insertCell().textContent=m?m[1]:t("a_other");
        row.insertCell().appendChild(h("span",{class:"mono",text:m?m[2]:l.slice(4)}));
        var c=row.insertCell();
        if(m){
          used[m[1]]=1;
          if(!/^and /.test(m[2]))c.appendChild(h("button",{class:"ctl",text:t("a_remove"),onclick:function(){
            postCmd("acl "+m[1]+" off").then(load).catch(function(){})}}));
        }
      });
      if(!lines.length)tb.insertRow().insertCell().textContent=t("a_none");
      if(!$("a-n").value)for(var n=1;n<=64;n++)if(!used[n]){$("a-n").value=n;break;}
    });
  }
  $("a-add").addEventListener("click",function(){
    var n=$("a-n").value.trim(),rest=$("a-rest").value.trim();
    if(!n||!rest)return;
    postCmd("acl "+n+" "+rest).then(function(){$("a-rest").value="";$("a-n").value="";load()}).catch(function(){});
  });
  $("a-rest").addEventListener("keydown",function(e){if(e.key==="Enter")$("a-add").click()});
  /* On request only: acl show clears the meters' exceeded bits, and each call joins the command history */
  $("a-show").addEventListener("click",function(){
    postCmd("acl show",true).then(function(r){var o=$("a-out");o.textContent=(r.body||"").trim();o.style.display=""}).catch(function(){});
  });
  var hk=tabHooks.l2,enter=hk&&hk.enter;
  if(hk)hk.enter=function(){if(enter)enter.apply(this,arguments);load().catch(function(){})};
  i18nApply();
})();
</script>
"""

MIME = {
    ".html": "text/html",
    ".ico": "image/svg+xml",  # RTLPlayground's favicon.ico is an SVG
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".js": "text/javascript",
    ".css": "text/css",
    ".txt": "text/plain",
}


def c_bytes(data):
    lines = []
    for i in range(0, len(data), 16):
        lines.append("  " + "".join("0x%02x," % b for b in data[i:i + 16]))
    return "\n".join(lines)


def main():
    out, config = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    files = []
    for d in sys.argv[3:]:
        files += sorted(p for p in pathlib.Path(d).iterdir() if p.is_file() and p.suffix in MIME)
    src = ["// Generated by tools/embed_web.py, do not edit", "#include \"web_assets.h\"", ""]
    table = []
    for k, p in enumerate(files):
        raw = p.read_bytes()
        if p.name == "index.html":
            raw = raw.replace(b"</body>", (NAV_GRAPHS + SYSTEM_HTTPS + DOT1X + ACL).encode() + b"</body>", 1)
        data = gzip.compress(raw, compresslevel=9, mtime=0)
        src.append("static const uint8_t f%d[%d] = {\n%s\n};" % (k, len(data), c_bytes(data)))
        table.append((("/" + p.name), k, len(data), MIME[p.suffix]))
        if p.name == "index.html":
            table.append(("/", k, len(data), MIME[p.suffix]))
    src.append("")
    src.append("const struct web_asset web_assets[] = {")
    for name, k, n, mime in table:
        src.append('  { "%s", "%s", f%d, %d, 1 },' % (name, mime, k, n))
    src.append("  { 0 }")
    src.append("};")
    src.append("")
    text = config.read_text().replace("\\", "\\\\").replace('"', '\\"')
    src.append("const char rtl_default_config[] =")
    for line in text.splitlines(keepends=True):
        src.append('  "%s"' % line.replace("\n", "\\n"))
    src.append("  ;")
    new = "\n".join(src) + "\n"
    if not out.exists() or out.read_text() != new:
        out.write_text(new)


if __name__ == "__main__":
    main()
