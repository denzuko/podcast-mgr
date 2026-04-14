#!/usr/bin/env python3
"""
scripts/ast_filter.py — transform clang -ast-dump=json → ast.json

Reads the raw clang AST JSON (25 MB, 147K nodes) and emits a compact
structured summary (13 KB, 17 functions) that policy/ast.rego consumes.

This is a pure transformation step, not policy logic.  Rego handles
the assertions; this handles the shape of the input.

Usage:
    python3 scripts/ast_filter.py [--check] [raw.json] [out.json]

    --check   Exit 1 if out.json would change (stale-check for CI)
    raw.json  Default: /tmp/podcast_mgr_ast_raw.json
    out.json  Default: ast.json
"""

import json
import sys
import pathlib
import argparse

# ── Functions we track ────────────────────────────────────────────────────

OUR_FUNCTIONS = frozenset({
    "resolve_config_path", "load_feeds_xml",  "write_feeds_xml",
    "xml_str_escape",      "validate_fields", "sv_is_blank",
    "parse_id",            "send_response",   "render_notice",
    "render_list",         "render_form",     "render_error",
    "render_shell",        "kxml_sv",         "kxml_input",
    "kxml_select",         "main",
})

KHTML_CALLS = frozenset({
    "khtml_open", "khtml_close", "khtml_elem",
    "khtml_attr", "khtml_closeelem", "khtml_puts",
})

KXML_CALLS = frozenset({
    "kxml_open",    "kxml_close",   "kxml_pushtag",
    "kxml_poptag",  "kxml_attr",    "kxml_putc",
    "kxml_putn",
})

# ── AST traversal ─────────────────────────────────────────────────────────

def collect_calls(node: dict, seen: set | None = None) -> set:
    """Collect all directly-called function names in a subtree."""
    if seen is None:
        seen = set()
    if not isinstance(node, dict):
        return seen
    if node.get("kind") == "CallExpr":
        for child in node.get("inner", []):
            if child.get("kind") in ("DeclRefExpr", "ImplicitCastExpr"):
                ref = (child if child.get("kind") == "DeclRefExpr"
                       else child.get("inner", [{}])[0])
                name = (ref.get("referencedDecl", {}).get("name")
                        or ref.get("name", ""))
                if name:
                    seen.add(name)
                break
    for v in node.values():
        if isinstance(v, dict):
            collect_calls(v, seen)
        elif isinstance(v, list):
            for item in v:
                collect_calls(item, seen)
    return seen


def collect_goto_labels(node: dict, labels: set | None = None) -> set:
    """Collect all goto label names used in a subtree."""
    if labels is None:
        labels = set()
    if not isinstance(node, dict):
        return labels
    if node.get("kind") == "GotoStmt":
        for child in node.get("inner", []):
            if child.get("kind") == "LabelDecl":
                labels.add(child.get("name", ""))
    for v in node.values():
        if isinstance(v, dict):
            collect_goto_labels(v, labels)
        elif isinstance(v, list):
            for item in v:
                collect_goto_labels(item, labels)
    return labels


def extract_params(fn_node: dict) -> list:
    """Extract parameter name and type info from a FunctionDecl node."""
    return [
        {
            "name": p.get("name", ""),
            "type": p.get("type", {}).get("qualType", ""),
            "is_const_ptr": (
                "const" in p.get("type", {}).get("qualType", "")
                and "*" in p.get("type", {}).get("qualType", "")
            ),
        }
        for p in fn_node.get("inner", [])
        if p.get("kind") == "ParmVarDecl"
    ]


# ── Main transform ────────────────────────────────────────────────────────

def transform(raw: dict) -> dict:
    """Produce the ast.json structure from a raw clang AST."""
    functions = []

    for fn in raw.get("inner", []):
        if fn.get("kind") != "FunctionDecl":
            continue
        name = fn.get("name", "")
        if name not in OUR_FUNCTIONS:
            continue

        calls = collect_calls(fn)
        gotos = collect_goto_labels(fn)

        functions.append({
            "name":             name,
            "return_type":      fn.get("type", {}).get("qualType", ""),
            "params":           extract_params(fn),
            "calls_malloc":     "malloc" in calls,
            "calls_khtml":      bool(calls & KHTML_CALLS),
            "calls_kxml":       bool(calls & KXML_CALLS),
            "calls_khttp_free": "khttp_free" in calls,
            "khtml_calls":      sorted(calls & KHTML_CALLS),
            "kxml_calls":       sorted(calls & KXML_CALLS),
            "all_calls":        sorted(calls),
            "goto_labels":      sorted(gotos),
        })

    return {
        "source_file":    "main.c",
        "functions":      functions,
        "function_names": [f["name"] for f in functions],
    }


# ── CLI ───────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Transform clang AST JSON → ast.json for Rego policy checks"
    )
    parser.add_argument("raw",  nargs="?",
                        default="/tmp/podcast_mgr_ast_raw.json",
                        help="clang raw AST JSON (default: /tmp/podcast_mgr_ast_raw.json)")
    parser.add_argument("out",  nargs="?",
                        default="ast.json",
                        help="output file (default: ast.json)")
    parser.add_argument("--check", action="store_true",
                        help="exit 1 if out would change (stale check)")
    args = parser.parse_args()

    raw_path = pathlib.Path(args.raw)
    out_path = pathlib.Path(args.out)

    if not raw_path.exists():
        print(f"Error: {raw_path} not found — run clang AST dump first",
              file=sys.stderr)
        return 1

    try:
        with raw_path.open() as f:
            raw = json.load(f)
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON in {raw_path}: {e}", file=sys.stderr)
        return 1

    output   = transform(raw)
    new_text = json.dumps(output, indent=2)

    if args.check:
        existing = out_path.read_text() if out_path.exists() else ""
        if existing.strip() != new_text.strip():
            print(f"{out_path} is stale — run: sh scripts/gen_ast.sh",
                  file=sys.stderr)
            return 1
        print(f"{out_path} is up to date")
        return 0

    out_path.write_text(new_text)
    print(f"{out_path} written — {len(output['functions'])} functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
