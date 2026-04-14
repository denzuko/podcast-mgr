# scripts/nob_ast_filter.jq
# Transform clang -ast-dump=json of nob.c -> nob_ast.json for policy/nob_ast.rego

def callee_name:
  if   .kind == "DeclRefExpr"      then .referencedDecl.name // empty
  elif .kind == "ImplicitCastExpr" then (.inner[0] | callee_name)
  else empty
  end;

def calls_in: .. | objects | select(.kind == "CallExpr") | .inner[0] | callee_name;

{
  source_file: "nob.c",
  functions: [
    .inner[]
    | select(.kind == "FunctionDecl")
    | select(.name | IN(
        "require_tool","cmd_build","cmd_test_unit","cmd_test_e2e","cmd_test",
        "cmd_ast","cmd_nob_ast","cmd_cflow","cmd_sbom","cmd_sarif","cmd_vex","cmd_policy",
        "cmd_clean","cmd_help","main"))
    | {
        name: .name,
        return_type: .type.qualType,
        calls_system:  ([calls_in] | any(. == "system")),
        calls_popen:   ([calls_in] | any(. == "popen")),
        calls_exec:    ([calls_in] | any(IN("execve","execl","execvp","execle","execlp"))),
        calls_fork:    ([calls_in] | any(. == "fork")),
        calls_dlopen:  ([calls_in] | any(. == "dlopen")),
        calls_unlink:  ([calls_in] | any(. == "unlink")),
        nob_calls:     ([calls_in | select(startswith("nob_"))] | unique | sort),
        all_calls:     ([calls_in] | unique | sort)
      }
  ],
  function_names: [
    .inner[] | select(.kind == "FunctionDecl")
    | select(.name | IN(
        "require_tool","cmd_build","cmd_test_unit","cmd_test_e2e","cmd_test",
        "cmd_ast","cmd_nob_ast","cmd_cflow","cmd_sbom","cmd_sarif","cmd_vex","cmd_policy",
        "cmd_clean","cmd_help","main"))
    | .name
  ]
}
