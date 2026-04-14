# podcast-mgr — Code Flow Diagram

Generated from `cflow` static call graph analysis of `main.c`.

## System Architecture

```mermaid
graph TD
    Browser -->|HTTP| NGINX[nginx / lighttpd]
    NGINX -->|FastCGI| CGI[index.cgi]
    CRON[cron job] -->|reads| XML[(feeds.xml)]
    CGI -->|reads/writes| XML

    style Browser fill:#e2e8f0
    style CRON fill:#e2e8f0
    style XML fill:#fef9c3
```

## Worker Lifecycle

```mermaid
flowchart TD
    START([process start]) --> INIT[arena_init\n4 MB root arena]
    INIT --> PAGES[build pages[] from\nROUTES table]
    PAGES --> FCGI[khttp_fcgi_init\nregister keys + routes]
    FCGI --> PATH[resolve_config_path\nXDG_CONFIG_HOME / HOME]
    PATH --> LOAD[load_feeds_xml]
    LOAD --> SANDBOX[sandbox_lockdown_rw\nseccomp / Capsicum / pledge]
    SANDBOX --> LOOP{khttp_fcgi_parse\naccept loop}

    LOOP -->|KCGI_OK| METHOD{method matches\nROUTES table?}
    METHOD -->|no| ERR_405[render_error\nMethod not allowed]
    METHOD -->|yes| DISPATCH{r.page}

    DISPATCH --> IDX[PAGE_INDEX\nrender_shell khtml]
    DISPATCH --> LIST[PAGE_LIST\nrender_list kxml]
    DISPATCH --> ADD[PAGE_ADD\nrender_form kxml]
    DISPATCH --> EDIT[PAGE_EDIT\nparse_id → render_form]
    DISPATCH --> SAVE[PAGE_SAVE\nvalidate → upsert → write]
    DISPATCH --> DEL[PAGE_DELETE\nsoft-delete → write]

    IDX --> FREE[khttp_free]
    LIST --> FREE
    ADD --> FREE
    EDIT --> FREE
    SAVE --> FREE
    DEL --> FREE
    ERR_405 --> FREE
    FREE --> LOOP

    LOOP -->|done| CLEANUP[khttp_fcgi_free\narena_destroy]
    CLEANUP --> EXIT([exit 0])

    LOAD -->|fail| FATAL([exit 1])
    PATH -->|fail| FATAL
    FCGI -->|fail| FATAL
    SANDBOX -->|fail| FATAL
```

## load_feeds_xml

```mermaid
flowchart TD
    A[load_feeds_xml path] --> B{lstat — symlink?}
    B -->|yes| FAIL([return -1])
    B -->|no| C[fopen rb]
    C --> D{fstat — regular file?\nsize ≤ 512 KB?}
    D -->|no| FAIL
    D -->|yes| E[arena_alloc fsize+1\nfread into buf]
    E --> F[xml_parse_string buf]
    F -->|NULL| FAIL
    F -->|ok| G[xml_node_child_at root 0\n= subscriptions]
    G -->|NULL| EMPTY([return 0 — empty])
    G -->|ok| H[for each child node]
    H --> I[xml_node_attr title/url/scope/day/pull_time]
    I --> J[arena_alloc + memcpy\nsv_from_parts]
    J --> K[da_append db]
    K --> H
    H -->|done| L[xml_node_free root]
    L --> OK([return 0])
```

## PAGE_SAVE — upsert flow

```mermaid
flowchart TD
    A[POST /save] --> B[parse_id → is_update?]
    B --> C[validate_fields\npresence · length · enum]
    C -->|error| E[render_error 400]
    C -->|ok| F[arena_alloc + memcpy\nkp->val → sv_from_parts]
    F --> G{is_update?}
    G -->|yes| H[snapshot old = db.items-id\ndb.items-id = p]
    G -->|no| I[da_append db p]
    H --> J[write_feeds_xml]
    I --> J
    J -->|fail| K[restore old / old_count\nrender_error 500]
    J -->|ok| L[render_list NOTICE_OK\nSaved.]
```

## write_feeds_xml — atomic write

```mermaid
flowchart TD
    A[write_feeds_xml path db] --> B[xml_string_new]
    B --> C[append XML header\nsubscriptions open]
    C --> D[for each non-deleted entry]
    D --> E[xml_string_append podcast attrs\nxml_str_escape values]
    E --> D
    D -->|done| F[append subscriptions close]
    F --> G[fopen path.tmp wb]
    G -->|fail| X([return -1\nxml_string_free])
    G -->|ok| H[fwrite + fflush + fclose]
    H -->|err| Y[unlink path.tmp\nreturn -1]
    H -->|ok| I[rename path.tmp → path]
    I -->|fail| Y
    I -->|ok| J[xml_string_free\nfree tmp\nreturn 0]
```

## render_form — data-driven field loop

```mermaid
flowchart TD
    A[render_form r p id] --> B[kxml_open]
    B --> C[emit div + h2 + form open]
    C --> D{is_edit?}
    D -->|yes| E[khttp_puts hidden id input]
    D -->|no| F
    E --> F[for i in FIELDS 0..4]
    F --> G[kxml label]
    G --> H{FIELDS-i .kind}
    H -->|INPUT_SELECT| I[kxml_select\nopts from FIELDS-i]
    H -->|INPUT_TEXT\nINPUT_URL| J[kxml_input\nkhttp_puts void element]
    I --> K[kxml_poptag div]
    J --> K
    K --> F
    F -->|done| L[Save + Cancel buttons]
    L --> M[kxml_close]
```

## Renderer split: khtml vs kxml

```mermaid
flowchart LR
    subgraph khtml ["khtml — SPA root only"]
        RS[render_shell\nDOCTYPE · head · nav · main]
    end
    subgraph kxml ["kxml — all htmx partials"]
        RL[render_list]
        RF[render_form]
        RE[render_error]
        RN[render_notice]
        KS[kxml_select]
        KI[kxml_input*]
    end
    subgraph note ["* void element exception"]
        KI2[khttp_puts for input tag\nkxml cannot self-close]
    end
    KI --> KI2
```
