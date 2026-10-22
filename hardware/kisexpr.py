"""Minimal reader and writer for KiCad S-expression files, and symbol-library
helpers: find a symbol, flatten (extends ...), list its pins."""
import re

_TOK = re.compile(r'\s*(?:(\()|(\))|"((?:[^"\\]|\\.)*)"|([^\s()"]+))', re.S)


class Q(str):
    """A quoted string, as opposed to a bare atom"""


def parse(text):
    stack, cur = [], []
    for m in _TOK.finditer(text):
        if m.group(1):
            stack.append(cur)
            cur = []
        elif m.group(2):
            done, cur = cur, stack.pop()
            cur.append(done)
        elif m.group(3) is not None:
            cur.append(Q(re.sub(r'\\(.)', r'\1', m.group(3))))
        elif m.group(4):
            cur.append(m.group(4))
    return cur[0] if len(cur) == 1 else cur


def dump(node, indent=0):
    if not isinstance(node, list):
        if isinstance(node, Q):
            return '"' + node.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'
        return str(node)
    simple = all(not isinstance(x, list) for x in node)
    if simple:
        return "(" + " ".join(dump(x) for x in node) + ")"
    pad = "\t" * (indent + 1)
    head = [dump(x) for x in node if not isinstance(x, list)]
    body = [pad + dump(x, indent + 1) for x in node if isinstance(x, list)]
    return "(" + " ".join(head) + "\n" + "\n".join(body) + "\n" + "\t" * indent + ")"


def find(node, key):
    return [x for x in node if isinstance(x, list) and x and x[0] == key]


def first(node, key):
    r = find(node, key)
    return r[0] if r else None


def library_symbol(lib, name):
    """The named symbol from a parsed .kicad_sym, with (extends) flattened"""
    syms = {s[1]: s for s in find(lib, "symbol")}
    sym = syms[name]
    parent = first(sym, "extends")
    if not parent:
        return [x for x in sym]
    base = library_symbol(lib, parent[1])
    props = {p[1]: p for p in find(sym, "property")}
    out = ["symbol", Q(name)]
    for x in base[2:]:
        if isinstance(x, list) and x[0] == "property" and x[1] in props:
            out.append(props.pop(x[1]))
        elif isinstance(x, list) and x[0] == "symbol":
            sub = list(x)
            sub[1] = Q(name + sub[1][len(parent[1]):])
            out.append(sub)
        else:
            out.append(x)
    for p in props.values():
        out.append(p)
    # anything else the child sets (e.g. pin_names), except extends
    for x in sym[2:]:
        if isinstance(x, list) and x[0] not in ("property", "extends", "symbol"):
            out = [y for y in out if not (isinstance(y, list) and y[0] == x[0])] + [x]
    return out


def pins(sym, unit=1):
    """[(number, name, type, x, y, angle)] for one unit, body style 1, library coordinates"""
    res = []
    for sub in find(sym, "symbol"):
        u, style = map(int, sub[1].rsplit("_", 2)[-2:])
        if u not in (0, unit) or style not in (0, 1):
            continue
        for p in find(sub, "pin"):
            at = first(p, "at")
            res.append((first(p, "number")[1], first(p, "name")[1], p[1],
                        float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0))
    return res


def uid(key):
    """A stable UUID for a schematic or board item, so regenerating a file
    changes only what changed"""
    import uuid
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "https://github.com/wrcrooks/RTLPlayground/modchip/" + key))
