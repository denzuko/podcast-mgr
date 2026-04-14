#!/bin/sh
##
# scripts/gen_ast.sh — filter clang AST raw JSON -> ast.json
#
# Modes:
#   sh scripts/gen_ast.sh            Full run: clang dump + python filter
#   sh scripts/gen_ast.sh --filter   Python filter only (raw file must exist)
#   sh scripts/gen_ast.sh --check    Verify ast.json is up to date; exit 1 if stale
#
# When called from ./nob ast, nob runs clang itself (stdout -> RAW),
# then calls this script with --filter for the python step only.
##

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

RAW="/tmp/podcast_mgr_ast_raw.json"
OUT="ast.json"
MODE="full"

case "${1:-}" in
    --filter) MODE="filter" ;;
    --check)  MODE="check"  ;;
esac

# Step 1: clang dump (full mode only) ------------------------------------
if [ "$MODE" = "full" ]; then
    if ! command -v clang > /dev/null 2>&1; then
        echo "Error: clang not found" >&2; exit 1
    fi
    # clang exits 1 on missing headers but still emits valid AST JSON.
    clang -Xclang -ast-dump=json \
          -fsyntax-only -fno-color-diagnostics \
          -w -ferror-limit=0 \
          -D_GNU_SOURCE -I. main.c > "$RAW" 2>/dev/null || true
    if [ ! -s "$RAW" ]; then
        echo "Error: clang produced no output" >&2; exit 1
    fi
fi

if [ "$MODE" != "full" ] && [ ! -f "$RAW" ]; then
    echo "Error: $RAW not found — run 'sh scripts/gen_ast.sh' first" >&2
    exit 1
fi

# Step 2: python3 filter -------------------------------------------------
if ! command -v python3 > /dev/null 2>&1; then
    echo "Error: python3 not found" >&2; exit 1
fi

python3 - "$MODE" "$RAW" "$OUT" << 'PYEOF'
import json, sys, pathlib

mode, raw_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

OUR_FUNCTIONS = {
    'resolve_config_path','load_feeds_xml','write_feeds_xml',
    'xml_str_escape','validate_fields','sv_is_blank',
    'parse_id','send_response','render_notice','render_list',
    'render_form','render_error','render_shell','kxml_sv',
    'kxml_input','kxml_select','main'
}
KHTML_CALLS = {'khtml_open','khtml_close','khtml_elem','khtml_attr','khtml_closeelem','khtml_puts'}
KXML_CALLS  = {'kxml_open','kxml_close','kxml_pushtag','kxml_poptag','kxml_attr','kxml_putc','kxml_putn'}

def collect_calls(node, seen=None):
    if seen is None: seen = set()
    if not isinstance(node, dict): return seen
    if node.get('kind') == 'CallExpr':
        for child in node.get('inner', []):
            if child.get('kind') in ('DeclRefExpr','ImplicitCastExpr'):
                ref = child if child.get('kind')=='DeclRefExpr' else child.get('inner',[{}])[0]
                name = ref.get('referencedDecl',{}).get('name') or ref.get('name','')
                if name: seen.add(name)
                break
    for v in node.values():
        if isinstance(v,dict): collect_calls(v,seen)
        elif isinstance(v,list): [collect_calls(i,seen) for i in v]
    return seen

def collect_gotos(node, labels=None):
    if labels is None: labels = set()
    if not isinstance(node, dict): return labels
    if node.get('kind') == 'GotoStmt':
        for child in node.get('inner',[]):
            if child.get('kind') == 'LabelDecl': labels.add(child.get('name',''))
    for v in node.values():
        if isinstance(v,dict): collect_gotos(v,labels)
        elif isinstance(v,list): [collect_gotos(i,labels) for i in v]
    return labels

try:
    with open(raw_path) as f: ast = json.load(f)
except Exception as e:
    print(f'Error: {e}', file=sys.stderr); sys.exit(1)

functions = []
for fn in ast.get('inner',[]):
    if fn.get('kind') != 'FunctionDecl': continue
    name = fn.get('name','')
    if name not in OUR_FUNCTIONS: continue
    calls = collect_calls(fn)
    gotos = collect_gotos(fn)
    params = [{'name':p.get('name',''),'type':p.get('type',{}).get('qualType',''),
               'is_const_ptr':'const' in p.get('type',{}).get('qualType','') and
                              '*' in p.get('type',{}).get('qualType','')}
              for p in fn.get('inner',[]) if p.get('kind')=='ParmVarDecl']
    functions.append({
        'name': name,
        'return_type': fn.get('type',{}).get('qualType',''),
        'params': params,
        'calls_malloc':     'malloc' in calls,
        'calls_khtml':      bool(calls & KHTML_CALLS),
        'calls_kxml':       bool(calls & KXML_CALLS),
        'calls_khttp_free': 'khttp_free' in calls,
        'khtml_calls':      sorted(calls & KHTML_CALLS),
        'kxml_calls':       sorted(calls & KXML_CALLS),
        'all_calls':        sorted(calls),
        'goto_labels':      sorted(gotos),
    })

output = {'source_file':'main.c','functions':functions,'function_names':[f['name'] for f in functions]}
new_text = json.dumps(output, indent=2)

if mode == 'check':
    existing = pathlib.Path(out_path).read_text() if pathlib.Path(out_path).exists() else ''
    if existing.strip() != new_text.strip():
        print(f'{out_path} is stale — run: sh scripts/gen_ast.sh', file=sys.stderr); sys.exit(1)
    print(f'{out_path} is up to date')
else:
    with open(out_path,'w') as f: f.write(new_text)
    print(f'{out_path} written — {len(functions)} functions')
PYEOF
