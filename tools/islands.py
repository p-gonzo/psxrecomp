#!/usr/bin/env python3
"""Island execution profile: rewrite generated game C for constrained hosts.

psxrecomp's generated functions keep every MIPS register in the CPUState
(cpu->gpr[]) and return to the dispatcher on every call and return
(continuation-passing style). That keeps any block resumable from the
dispatcher, at a cost: register traffic through memory around almost every
instruction (a guest store through a byte pointer may alias cpu->gpr, so the
compiler cannot keep them in host registers), and two dispatcher round trips
per guest call.

This post-pass rewrites each game function into an "island":

- MIPS registers live in C locals (_g1.._g31), loaded from cpu->gpr once at
  entry (before the resume switch, so both entry paths start from the CPU
  state) and written back (only the ones the function writes) wherever
  something outside the function may look at or change them: before every
  return, around a slow block edge (interrupt delivery may unwind), and around
  helpers that read or write the register file (gte_execute, syscalls, alias
  bodies, anything not on the safe list). After such a call the locals are
  reloaded.
- A `jal` to a known game function whose return label is in the same function
  becomes a direct C call: write back, call, and if the callee came back with
  cpu->pc == the return address, reload and continue inline. Anything else
  (a tail transfer, a thread switch, an unwound exception) falls back to the
  dispatcher exactly as before: the state is already in the CPUState and every
  return address is a resume point. Direct calls are taken only while no game
  code word has ever changed (psx_island_direct_ok, runtime/include/psx_island.h);
  otherwise the dispatcher's code-validity checks stay in charge.
- Block edges use the lean-host horizon inline; the register write-back and
  reload happen only on the slow path.

The rewrite is mechanical over psxrecomp's own output (every register access is
literally cpu->gpr[N]); semantics are the generated code's. The faithful
sources are not modified: islands go to a separate directory that a build can
select instead (runtime/include/psx_island.h documents the runtime contract).

    python3 psxrecomp/tools/islands.py --in generated --out build-3ds/islands \\
        --decls generated/SLUS_010.66_decls.h
"""
import argparse
import hashlib
import os
import re
import sys

FUNC_RE = re.compile(r"^void func_([0-9A-F]{8})\(CPUState\* cpu\)$")
GPR_RE = re.compile(r"cpu->gpr\[(\d+)\]")
WRITE_RE = re.compile(r"cpu->gpr\[(\d+)\]\s*(?:=(?!=)|\+=|-=|\|=|&=|\^=|<<=|>>=)")
LABEL_RE = re.compile(r"^block_([0-9A-F]{8}):")
LINK_RE = re.compile(r"_g31 = 0x([0-9A-F]{8})u;\s*/\* jal link before delay slot \*/")
CPS_PC_RE = re.compile(r"^(\s*)cpu->pc = 0x([0-9A-F]{8})u;\s*$")
CPS_JAL_RET_RE = re.compile(r"^\s*return;\s*/\* CPS jal -> 0x([0-9A-F]{8}) \*/")
RETURN_RE = re.compile(r"\breturn;")
EDGE_RE = re.compile(r"\bPSX_BLOCK_EDGE\(cpu, ")
CALL_CPU_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\(cpu\b")

# Helpers called with `cpu` that neither read nor write cpu->gpr nor unwind
# (checked against their definitions: timing, memory and pure merge helpers).
SAFE_CALLS = {
    "psx_cyc_step", "psx_icache_fetch", "psx_cyc_load_word", "psx_cyc_load_half",
    "psx_cyc_load_byte", "psx_cyc_lwc2_read", "PSX_STORE_WORD", "PSX_STORE_HALF",
    "PSX_STORE_BYTE", "psx_slice_block", "psx_muldiv_set", "psx_muldiv_stall",
    "psx_gte_stall", "psx_gte_read", "gte_write_data", "gte_read_data",
    "gte_write_ctrl", "gte_read_ctrl", "psx_lwl", "psx_lwr", "psx_swl", "psx_swr",
    "psx_pgxp_operation_allowed", "psx_pgxp_operation_memory",
    "psx_pgxp_operation_begin", "psx_pgxp_operation_end",
    "PSX_BLOCK_EDGE",   # rewritten to ISL_EDGE, which handles write-back itself
}


def load_functions(decls_path):
    funcs = set()
    for line in open(decls_path, errors="replace"):
        m = re.match(r"^\s*void func_([0-9A-F]{8})\(CPUState\* cpu\);", line)
        if m:
            funcs.add(m.group(1))
    return funcs


def transform_function(header, body, funcs, stats):
    """body: list of lines between the opening '{' line and the closing '}'."""
    text = "".join(body)
    regs = sorted({int(n) for n in GPR_RE.findall(text)} - {0})
    writes = sorted({int(n) for n in WRITE_RE.findall(text)} - {0})
    labels = {m.group(1) for line in body for m in [LABEL_RE.match(line)] if m}
    loads = " ".join(f"_g{r} = cpu->gpr[{r}];" for r in regs)
    stores = " ".join(f"cpu->gpr[{r}] = _g{r};" for r in writes)
    out = []
    decl = "    uint32_t _g0 = 0u" + "".join(f", _g{r} = cpu->gpr[{r}]" for r in regs) + ";\n"
    out.append(decl)
    out.append(f"#define ISL_FLUSH() do {{ {stores} }} while (0)\n")
    out.append(f"#define ISL_RELOAD() do {{ {loads} }} while (0)\n")
    out.append("    (void)_g0;\n")
    # One slow block-edge path per function (see ISL_EDGE_AT in psx_island.h):
    # each edge is a compare plus a jump into it, not an inline write-back,
    # call and reload (135k edges in THPS2: the difference is ~8 MB of code).
    out.append("    uint32_t _isl_pc = 0u; void *_isl_back = 0; (void)_isl_pc; (void)_isl_back;\n")
    edge_n = 0

    lines = [GPR_RE.sub(lambda m: "_g" + m.group(1), l) for l in body]
    pending_ret = None
    i = 0
    while i < len(lines):
        line = lines[i]
        m = LINK_RE.search(line)
        if m:
            pending_ret = m.group(1)
        if LABEL_RE.match(line):
            pending_ret = None
        # CPS jal: `cpu->pc = 0xT;` + `return;  /* CPS jal -> 0xT */`
        mp = CPS_PC_RE.match(line)
        if mp and i + 1 < len(lines):
            # Main bodies carry a (disabled) native-tail-chain block between
            # the pc assignment and the return; islands do not use it.
            k = i + 1
            if lines[k].startswith("#if defined(PSX_NATIVE_TAIL_CHAINS)"):
                while k < len(lines) and not lines[k].startswith("#endif"):
                    k += 1
                k += 1
            mj = CPS_JAL_RET_RE.match(lines[k]) if k < len(lines) else None
            if mj and mj.group(1) == mp.group(2):
                ind, target = mp.group(1), mp.group(2)
                if target in funcs and pending_ret and pending_ret in labels:
                    out.append(
                        f"{ind}ISL_FLUSH();\n"
                        f"{ind}if (psx_island_direct_ok()) {{\n"
                        f"{ind}    psx_island_call(cpu, 0x{target}u, func_{target});\n"
                        f"{ind}    if (psx_island_returned(cpu, 0x{pending_ret}u)) {{ psx_island_resume(cpu, 0x{pending_ret}u); ISL_RELOAD(); goto block_{pending_ret}; }}\n"
                        f"{ind}    return;\n"
                        f"{ind}}}\n"
                        f"{ind}cpu->pc = 0x{target}u;\n"
                        f"{ind}return;  /* CPS jal -> 0x{target} (island fallback) */\n")
                    stats["direct_calls"] += 1
                    i = k + 1
                    continue
                stats["cps_jal_kept"] += 1
        if EDGE_RE.search(line):
            # PSX_BLOCK_EDGE(cpu, X); -> ISL_EDGE_AT(cpu, X, n); (one per line)
            line = EDGE_RE.sub(f"ISL_EDGE_AT_{edge_n}(cpu, ", line)
            line = re.sub(rf"ISL_EDGE_AT_{edge_n}\(cpu, ([^;]*)\);",
                          lambda mm: f"ISL_EDGE_AT(cpu, {mm.group(1)[:-1] if mm.group(1).endswith(')') else mm.group(1)}, {edge_n});",
                          line)
            edge_n += 1
        calls = set(CALL_CPU_RE.findall(line))
        unsafe = {c for c in calls if c not in SAFE_CALLS and not c.startswith("ISL_EDGE")}
        if unsafe:
            stats["unsafe_sites"] += 1
            for c in unsafe:
                stats["unsafe:" + (c if not c.startswith("psx_alias_body_") else "psx_alias_body_*")] += 1
            assigned = sorted({int(n) for n in re.findall(r"\b_g(\d+)\s*(?:=(?!=)|\+=|-=|\|=|&=|\^=)", line)} - {0})
            after = " ".join(f"cpu->gpr[{r}] = _g{r};" for r in assigned)
            # Registers were written back before the call, and the call may
            # have changed them (a tail call into a split piece: `f(cpu);
            # return;`): a return on this line must not write back again.
            line = line.rstrip("\n")
            out.append(f"ISL_FLUSH(); {line} {after} ISL_RELOAD();\n")
            i += 1
            continue
        if RETURN_RE.search(line):
            line = RETURN_RE.sub("{ ISL_FLUSH(); return; }", line)
            stats["returns"] += 1
        out.append(line)
        i += 1
    if edge_n:
        out.append("    if (0) {\n_isl_slow:\n"
                   "        ISL_FLUSH(); psx_block_edge_call(cpu, _isl_pc); ISL_RELOAD(); goto *_isl_back;\n"
                   "    }\n")
    out.append("#undef ISL_FLUSH\n#undef ISL_RELOAD\n")
    stats["functions"] += 1
    stats["regs_total"] += len(regs)
    return out


def transform_file(src, dst, funcs, stats):
    lines = open(src, errors="replace").readlines()
    out = []
    i = 0
    while i < len(lines):
        m = FUNC_RE.match(lines[i].rstrip("\n"))
        only_funcs = os.environ.get("PSX_ISLANDS_FUNCS", "")
        if m and only_funcs and m.group(1) not in only_funcs.split(","):
            m = None   # debugging: leave this function faithful
        if m and i + 1 < len(lines) and lines[i + 1].rstrip("\n") == "{":
            j = i + 2
            while j < len(lines) and lines[j].rstrip("\n") != "}":
                j += 1
            if j >= len(lines):
                raise SystemExit(f"{src}: unterminated function at line {i + 1}")
            out.append(lines[i])
            out.append(lines[i + 1])
            out.extend(transform_function(lines[i], lines[i + 2:j], funcs, stats))
            out.append(lines[j])
            i = j + 1
            continue
        out.append(lines[i])
        i += 1
    # The island contract (psx_island_direct_ok, ISL_EDGE) after the decls.
    for k, l in enumerate(out):
        if l.startswith('#include "psx_native_span.h"'):
            out.insert(k + 1, '#include "psx_island.h"\n')
            break
    else:
        out.insert(0, '#include "psx_island.h"\n')
    tmp = dst + ".tmp"
    with open(tmp, "w") as f:
        f.writelines(out)
    os.replace(tmp, dst)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", required=True, help="generated/ directory")
    ap.add_argument("--out", required=True)
    ap.add_argument("--decls", required=True)
    ap.add_argument("--glob", default=r".*_full_\d+\.c$")
    ap.add_argument("--only", default=os.environ.get("PSX_ISLANDS_ONLY", ""),
                    help="debugging: rewrite only these file indices (e.g. 0-107,150); "
                         "the rest are copied unchanged")
    a = ap.parse_args()
    funcs = load_functions(a.decls)
    os.makedirs(a.out, exist_ok=True)
    tool_hash = hashlib.sha256(open(__file__, "rb").read()).hexdigest()[:16]
    stamp_path = os.path.join(a.out, ".islands-stamp")
    stamps = {}
    if os.path.exists(stamp_path):
        for l in open(stamp_path):
            k, _, v = l.strip().partition(" ")
            stamps[k] = v
    stats = __import__("collections").Counter()
    names = sorted(n for n in os.listdir(a.src) if re.search(a.glob, n))
    only = None
    if a.only:
        only = set()
        for part in a.only.split(","):
            lo, _, hi = part.partition("-")
            only.update(range(int(lo), int(hi or lo) + 1))
    new_stamps = {}
    done = 0
    for n in names:
        src = os.path.join(a.src, n)
        st = os.stat(src)
        idx = int(re.search(r"_full_(\d+)\.c$", n).group(1))
        selected = only is None or idx in only
        key = f"{tool_hash}:{st.st_size}:{int(st.st_mtime)}:{int(selected)}:{os.environ.get('PSX_ISLANDS_FUNCS', '')}"
        new_stamps[n] = key
        dst = os.path.join(a.out, n)
        if stamps.get(n) == key and os.path.exists(dst):
            continue
        if selected:
            transform_file(src, dst, funcs, stats)
        else:
            import shutil
            shutil.copyfile(src, dst + ".tmp"); os.replace(dst + ".tmp", dst)
        done += 1
    with open(stamp_path, "w") as f:
        for k, v in sorted(new_stamps.items()):
            f.write(f"{k} {v}\n")
    print(f"islands: {done} of {len(names)} files rewritten into {a.out}")
    if done:
        print("islands: " + ", ".join(f"{k} {v}" for k, v in sorted(stats.items())))


if __name__ == "__main__":
    main()
