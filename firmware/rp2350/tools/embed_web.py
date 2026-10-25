#!/usr/bin/env python3
"""Embed RTLPlayground's web UI and default config in the RP2350 firmware.

  embed_web.py OUT.c CONFIG_TXT HTML_DIR [HTML_DIR...]

Writes the files of each HTML_DIR gzipped (RTLPlayground's html/, then the
mod chip's own web/), with the same names and MIME types as RTLPlayground's
html_data.c, plus "/" for index.html, and config.txt as rtl_default_config.
index.html gets a "Graphs" entry in its navigation, for dashboard.html, and
an HTTPS switch in System > Services; the mod chip's commands (https, snmp,
t3) join the ones the web UI keeps when it saves the config.
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
            raw = raw.replace(b"</body>", (NAV_GRAPHS + SYSTEM_HTTPS).encode() + b"</body>", 1)
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
