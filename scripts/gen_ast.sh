#!/bin/sh
##
# scripts/gen_ast.sh — generate ast.json from main.c via clang AST
#
# Requires: clang, python3
# Output:   ast.json (input for policy/ast.rego)
#
# Usage:
#   sh scripts/gen_ast.sh
#   sh scripts/gen_ast.sh --check   # exit 1 if ast.json would change
##

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

if ! command -v clang > /dev/null 2>&1; then
    echo "Error: clang not found — install clang to generate ast.json" >&2
    exit 1
fi

clang -Xclang -ast-dump=json \
      -fsyntax-only \
      -fno-color-diagnostics \
      -D_GNU_SOURCE \
      -I. \
      main.c 2>/dev/null > /tmp/podcast_mgr_ast_raw.json

python3 << 'PYEOF'
import json, sys

OUR_FUNCTIONS = {
    'resolve_config_path','load_feeds_xml','write_feeds_xml',
    'xml_str_escape','validate_fields','sv_is_blank',
    'parse_id','send_response','render_notice','render_list',
    'render_form','render_error','render_shell','kxml_sv',
    'kxml_input','kxml_select','main'
}

KHTML_CALLS = {
    'khtml_open','khtml_close','khtml_elem','khtml_attr',
    'khtml_closeelem','khtml_puts',
}
KXML_CALLS = {
    'kxml_open','kxml_close','kxml_pushtag','kxml_poptag',
    'kxml_attr','kxml_putc','kxml_putn',
}

def collect_calls(node, seen=None):
    if seen is None: seen = set()
    if not isinstance(node, dict): return seen
    if node.get('kind') == 'CallExpr':
        for child in node.get('inner', []):
            if child.get('kind') in ('DeclRefExpr', 'ImplicitCastExpr'):
                ref = child if child.get('kind') == 'DeclRefExpr' \
                      else child.get('inner', [{}])[0]
                name = (ref.get('referencedDecl', {}).get('name') or
                        ref.get('name', ''))
                if name: seen.add(name)
                break
    for v in node.values():
        if isinstance(v, dict): collect_calls(v, seen)
        elif isinstance(v, list):
            for item in v: collect_calls(item, seen)
    return seen

def collect_goto_labels(node, labels=None):
    if labels is None: labels = set()
    if not isinstance(node, dict): return labels
    if node.get('kind') == 'GotoStmt':
        for child in node.get('inner', []):
            if child.get('kind') == 'LabelDecl':
                labels.add(child.get('name', ''))
    for v in node.values():
        if isinstance(v, dict): collect_goto_labels(v, labels)
        elif isinstance(v, list):
            for item in v: collect_goto_labels(item, labels)
    return labels

with open('/tmp/podcast_mgr_ast_raw.json') as f:
    ast = json.load(f)

functions = []
for fn in ast.get('inner', []):
    if fn.get('kind') != 'FunctionDecl': continue
    name = fn.get('name', '')
    if name not in OUR_FUNCTIONS: continue

    calls = collect_calls(fn)
    gotos = collect_goto_labels(fn)
    params = [
        {
            'name': p.get('name', ''),
            'type': p.get('type', {}).get('qualType', ''),
            'is_const_ptr': (
                'const' in p.get('type', {}).get('qualType', '') and
                '*' in p.get('type', {}).get('qualType', '')
            ),
        }
        for p in fn.get('inner', [])
        if p.get('kind') == 'ParmVarDecl'
    ]
    functions.append({
        'name': name,
        'return_type': fn.get('type', {}).get('qualType', ''),
        'params': params,
        'calls_malloc':       'malloc' in calls,
        'calls_khtml':        bool(calls & KHTML_CALLS),
        'calls_kxml':         bool(calls & KXML_CALLS),
        'calls_khttp_free':   'khttp_free' in calls,
        'khtml_calls':        sorted(calls & KHTML_CALLS),
        'kxml_calls':         sorted(calls & KXML_CALLS),
        'all_calls':          sorted(calls),
        'goto_labels':        sorted(gotos),
    })

output = {
    'source_file':    'main.c',
    'functions':      functions,
    'function_names': [f['name'] for f in functions],
}

import os, sys
out_path = os.path.join(os.path.dirname(os.path.abspath('/tmp/podcast_mgr_ast_raw.json')).replace('/tmp',''), 'ast.json')

# Write to stdout for --check mode, or to ast.json directly
if '--check' in sys.argv:
    import pathlib
    existing = pathlib.Path('ast.json').read_text() if pathlib.Path('ast.json').exists() else ''
    new = json.dumps(output, indent=2)
    if existing.strip() != new.strip():
        print("ast.json is stale — run: sh scripts/gen_ast.sh", file=sys.stderr)
        sys.exit(1)
    else:
        print("ast.json is up to date")
else:
    with open('ast.json', 'w') as f:
        json.dump(output, f, indent=2)
    print(f"ast.json written — {len(functions)} functions")
PYEOF
