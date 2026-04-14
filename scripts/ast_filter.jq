# scripts/ast_filter.jq
# Transform clang -ast-dump=json -> ast.json summary for policy/ast.rego
#
# Handles two callee patterns:
#   CallExpr -> DeclRefExpr              (direct call)
#   CallExpr -> ImplicitCastExpr -> DeclRefExpr  (function pointer / implicit)

def callee_name:
  if   .kind == "DeclRefExpr"      then .referencedDecl.name // empty
  elif .kind == "ImplicitCastExpr" then (.inner[0] | callee_name)
  else empty
  end;

def calls_in:
  .. | objects | select(.kind == "CallExpr") | .inner[0] | callee_name;

def gotos_in:
  .. | objects | select(.kind == "GotoStmt")
  | .inner[0]? | select(.kind == "LabelDecl") | .name;

def params_of:
  [ .inner[]? | select(.kind == "ParmVarDecl")
    | { name: .name,
        type: .type.qualType,
        is_const_ptr: ((.type.qualType | contains("const"))
                   and (.type.qualType | contains("*"))) } ];

{
  source_file: "main.c",
  functions: [
    .inner[]
    | select(.kind == "FunctionDecl")
    | select(.name | IN(
        "resolve_config_path", "load_feeds_xml",  "write_feeds_xml",
        "xml_str_escape",      "validate_fields", "sv_is_blank",
        "parse_id",            "send_response",   "render_notice",
        "render_list",         "render_form",     "render_error",
        "render_shell",        "kxml_sv",         "kxml_input",
        "kxml_select",         "main"))
    | {
        name:             .name,
        return_type:      .type.qualType,
        params:           params_of,
        calls_malloc:     ([calls_in] | any(. == "malloc")),
        calls_khtml:      ([calls_in] | any(startswith("khtml_"))),
        calls_kxml:       ([calls_in] | any(startswith("kxml_"))),
        calls_khttp_free: ([calls_in] | any(. == "khttp_free")),
        khtml_calls:      ([calls_in | select(startswith("khtml_"))] | unique | sort),
        kxml_calls:       ([calls_in | select(startswith("kxml_"))]  | unique | sort),
        all_calls:        ([calls_in] | unique | sort),
        goto_labels:      ([gotos_in] | unique | sort)
      }
  ],
  function_names: [ .inner[]
    | select(.kind == "FunctionDecl")
    | select(.name | IN(
        "resolve_config_path", "load_feeds_xml",  "write_feeds_xml",
        "xml_str_escape",      "validate_fields", "sv_is_blank",
        "parse_id",            "send_response",   "render_notice",
        "render_list",         "render_form",     "render_error",
        "render_shell",        "kxml_sv",         "kxml_input",
        "kxml_select",         "main"))
    | .name ]
}
