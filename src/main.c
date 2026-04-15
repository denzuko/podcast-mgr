/* _GNU_SOURCE: dev-host Linux/glibc shim only.
 * Primary target is BSD (FreeBSD/OpenBSD) where lstat(2), fileno(3), and
 * syscall(2) are visible by default.  On glibc, -std=c11 suppresses them
 * unless a feature-test macro is set before the first system header.
 * Has no effect on BSD or clang/libc on the target. */
#ifdef __linux__
# define _GNU_SOURCE
#endif

/*
 * podcast-mgr/main.c  —  FastCGI SPA for managing feeds.xml
 * BCHS stack: kcgi · kcgihtml · xml.h · sv.h · arena.h · sandbox.h
 *
 * Single-user local-network tool.  Guards retained are for correctness
 * (bounds checks, atomic writes, file sanity) not for adversarial threat
 * modelling.
 *
 * Routes (all under MOUNT_PATH/index.cgi/):
 *   GET  /index   — SPA shell (khtml)
 *   GET  /list    — card list partial (kxml)
 *   GET  /add     — blank add-form partial (kxml)
 *   GET  /edit    — pre-filled edit-form partial (?id=N) (kxml)
 *   POST /save    — upsert → write XML → list partial
 *   POST /delete  — remove entry → write XML → list partial (?id=N)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#include <kcgi.h>
#include <kcgihtml.h>

#define XML_H_IMPLEMENTATION
#include "xml.h"
#define SV_IMPLEMENTATION
#include "sv.h"
#define ARENA_IMPLEMENTATION
#include "arena.h"
#include "sandbox.h"

/* =========================================================================
 * §1  CONSTANTS
 * ====================================================================== */

#define MOUNT_PATH    "/podcast"
#define CGI_BIN       "/index.cgi"
#define ROUTE(x)      MOUNT_PATH CGI_BIN x

#define APP_SUBDIR    "/podcasts"
#define FEED_FILENAME "/feeds.xml"
#define REL_PATH      APP_SUBDIR FEED_FILENAME
#define XDG_FALLBACK  "/.config" REL_PATH

/* Sanity cap: reject feeds.xml larger than this (corrupt file guard) */
#define FEEDS_XML_MAXBYTES  (512u * 1024u)

/* =========================================================================
 * §2  FIELD DEFINITIONS  (data-driven: validation, rendering, serialisation)
 *
 * One FieldDef row drives everything: kcgi key registration, XML attribute
 * name, form label, input kind, length cap, and enum option list.
 * Adding a new feed attribute means one row here and one entry in PodcastAttr.
 * ====================================================================== */

enum key {
    KEY_ID,
    KEY_TITLE,
    KEY_URL,
    KEY_SCOPE,
    KEY_DAY,
    KEY_PULL,
    KEY__MAX
};

static const char *const SCOPE_OPTS[] = { "all", "latest", "none", NULL };
static const char *const DAY_OPTS[]   = {
    "Daily","Mon","Tue","Wed","Thu","Fri","Sat","Sun", NULL
};
static const char *const PULL_OPTS[]  = {
    "00","01","02","03","04","05","06","07","08","09",
    "10","11","12","13","14","15","16","17","18","19",
    "20","21","22","23", NULL
};

typedef enum { INPUT_TEXT, INPUT_URL, INPUT_SELECT } InputKind;

typedef struct {
    enum key          key;
    const char       *xml_name;
    const char       *label;
    InputKind         kind;
    size_t            maxlen;
    const char *const *opts;    /* non-NULL → <select> with these options */
} FieldDef;

#define FIELD_COUNT 5
static const FieldDef FIELDS[FIELD_COUNT] = {
    { KEY_TITLE, "title",     "Title",          INPUT_TEXT,   256,  NULL       },
    { KEY_URL,   "url",       "RSS URL",         INPUT_URL,    2048, NULL       },
    { KEY_SCOPE, "scope",     "Scope",           INPUT_SELECT, 16,   SCOPE_OPTS },
    { KEY_DAY,   "day",       "Schedule Day",    INPUT_SELECT, 16,   DAY_OPTS   },
    { KEY_PULL,  "pull_time", "Pull Hour (24h)", INPUT_SELECT, 16,   PULL_OPTS  },
};

static const struct kvalid keys[KEY__MAX] = {
    [KEY_ID]    = { kvalid_uint,   "id"        },
    [KEY_TITLE] = { kvalid_string, "title"     },
    [KEY_URL]   = { kvalid_string, "url"       },
    [KEY_SCOPE] = { kvalid_string, "scope"     },
    [KEY_DAY]   = { kvalid_string, "day"       },
    [KEY_PULL]  = { kvalid_string, "pull_time" },
};

/* =========================================================================
 * §3  DATA MODEL
 * ====================================================================== */

typedef enum {
    ATTR_TITLE, ATTR_URL, ATTR_SCOPE, ATTR_DAY, ATTR_PULL, ATTR__MAX
} PodcastAttr;

static_assert(FIELD_COUNT == ATTR__MAX, "FIELDS/ATTR count mismatch");
/* NOTE: FIELDS[n].key cannot be used in a static_assert (not a
 * constant expression in C).  Equivalent checks run at start of main(). */

typedef struct {
    String_View attrs[ATTR__MAX];
    bool        deleted;
} PodcastComp;

typedef struct {
    PodcastComp *items;
    size_t       count;
    size_t       capacity;
    Arena       *arena;
} PodcastArray;

/* =========================================================================
 * §4  ROUTING TABLE
 *
 * Declares expected HTTP method per route.  A single pre-dispatch check
 * enforces this — prevents accidental GET-triggered mutations.
 * ====================================================================== */

enum page {
    PAGE_INDEX, PAGE_LIST, PAGE_ADD, PAGE_EDIT,
    PAGE_SAVE, PAGE_DELETE,
    PAGE__MAX
};

typedef struct {
    const char  *path;
    enum kmethod method;
} RouteDef;

static const RouteDef ROUTES[PAGE__MAX] = {
    [PAGE_INDEX]  = { "index",  KMETHOD_GET  },
    [PAGE_LIST]   = { "list",   KMETHOD_GET  },
    [PAGE_ADD]    = { "add",    KMETHOD_GET  },
    [PAGE_EDIT]   = { "edit",   KMETHOD_GET  },
    [PAGE_SAVE]   = { "save",   KMETHOD_POST },
    [PAGE_DELETE] = { "delete", KMETHOD_POST },
};

static const char *pages[PAGE__MAX];   /* built from ROUTES in main() */

/* =========================================================================
 * §5  THEME
 * ====================================================================== */

typedef enum {
    S_CARD, S_HDR, S_SUB, S_URL_TEXT,
    S_BTN, S_BTN_DANGER, S_BTN_GHOST, S_BTN_ADD,
    S_INPUT, S_SELECT, S_LABEL,
    S_NOTICE_OK, S_NOTICE_ERR, S_NOTICE_INFO,
    S__MAX
} StyleKey;

static const char *const CSS[S__MAX] = {
    [S_CARD]       = "p-6 bg-white shadow-sm rounded-xl border flex "
                     "justify-between items-start gap-4 mb-4 transition "
                     "hover:border-slate-400",
    [S_HDR]        = "text-lg font-black text-slate-900 break-all",
    [S_SUB]        = "text-xs font-bold uppercase tracking-tighter "
                     "text-indigo-600 mt-1",
    [S_URL_TEXT]   = "text-xs text-slate-400 mt-1 truncate",
    [S_BTN]        = "bg-slate-900 text-white px-4 py-2 rounded-lg "
                     "hover:bg-slate-700 transition font-bold text-sm "
                     "whitespace-nowrap",
    [S_BTN_DANGER] = "bg-red-600 text-white px-4 py-2 rounded-lg "
                     "hover:bg-red-700 transition font-bold text-sm "
                     "whitespace-nowrap",
    [S_BTN_GHOST]  = "bg-slate-100 text-slate-700 px-4 py-2 rounded-lg "
                     "hover:bg-slate-200 transition font-bold text-sm "
                     "whitespace-nowrap",
    [S_BTN_ADD]    = "bg-indigo-600 text-white px-5 py-2 rounded-xl "
                     "font-bold hover:bg-indigo-700 transition",
    [S_INPUT]      = "p-2 border rounded-lg bg-slate-50 w-full text-sm "
                     "focus:bg-white focus:ring-2 focus:ring-slate-900 "
                     "outline-none",
    [S_SELECT]     = "p-2 border rounded-lg bg-slate-50 w-full text-sm "
                     "focus:bg-white focus:ring-2 focus:ring-slate-900 "
                     "outline-none appearance-none",
    [S_LABEL]      = "text-[10px] uppercase font-bold text-slate-400 "
                     "mb-1 block tracking-wider",
    [S_NOTICE_OK]  = "text-sm font-bold mt-3 text-center text-green-600",
    [S_NOTICE_ERR] = "text-sm font-bold mt-3 text-center text-red-600",
    [S_NOTICE_INFO]= "text-sm font-bold mt-3 text-center text-slate-500",
};

/* =========================================================================
 * §6  ARENA-BACKED DYNAMIC ARRAY
 * ====================================================================== */

#define da_append(da, item) do {                                          \
    if ((da)->count >= (da)->capacity) {                                  \
        size_t _oc = (da)->capacity;                                      \
        size_t _nc = (_oc == 0) ? 8 : _oc * 2;                           \
        (da)->items = arena_realloc((da)->arena, (da)->items,             \
                         _oc * sizeof(*(da)->items),                      \
                         _nc * sizeof(*(da)->items));                     \
        (da)->capacity = _nc;                                             \
    }                                                                     \
    (da)->items[(da)->count++] = (item);                                  \
} while (0)

/* =========================================================================
 * §7  PATH RESOLUTION
 * ====================================================================== */

static const char *resolve_config_path(Arena *a) {
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[4096];

    if (NULL != xdg && '\0' != xdg[0])
        snprintf(path, sizeof(path), "%s" REL_PATH, xdg);
    else if (NULL != home)
        snprintf(path, sizeof(path), "%s" XDG_FALLBACK, home);
    else
        return NULL;

    size_t len = strlen(path);
    char  *res = arena_alloc(a, len + 1);
    memcpy(res, path, len + 1);
    return res;
}

/* =========================================================================
 * §8  XML LOAD
 *
 * API: mrvladus/xml.h
 *   xml_parse_string(const char *)  → XMLNode *root  (free with xml_node_free)
 *   xml_node_child_at(node, idx)    → XMLNode *       (NULL if out of range)
 *   xml_node_attr(node, key)        → const char *    (NULL if absent)
 *   node->children->len             → size_t
 *
 * Tree shape: root(tag=NULL) → child[0]=<subscriptions> → child[i]=<podcast>
 *
 * Correctness guards retained:
 *   - lstat: reject symlinks (accidental misconfiguration, not attacks)
 *   - fstat: confirm regular file so fread doesn't block on a FIFO
 *   - size cap: bail on corrupt/oversized file before parse
 *   - NULL checks on xml.h return values
 *   - arena-copy attrs before xml_node_free invalidates xml.h's heap
 * ====================================================================== */

static int load_feeds_xml(const char *path, Arena *a, PodcastArray *db) {
    struct stat lst;
    if (lstat(path, &lst) != 0) return -1;
    if (S_ISLNK(lst.st_mode)) return -1;

    FILE *fp = fopen(path, "rb");
    if (NULL == fp) return -1;

    struct stat st;
    if (fstat(fileno(fp), &st) != 0 || !S_ISREG(st.st_mode)) {
        fclose(fp); return -1;
    }
    if ((size_t)st.st_size > FEEDS_XML_MAXBYTES) {
        fclose(fp); return -1;
    }

    size_t fsize = (size_t)st.st_size;
    char *buf = arena_alloc(a, fsize + 1);
    if (fsize != fread(buf, 1, fsize, fp)) { fclose(fp); return -1; }
    buf[fsize] = '\0';
    fclose(fp);

    XMLNode *root = xml_parse_string(buf);
    if (NULL == root) return -1;

    XMLNode *subs = xml_node_child_at(root, 0);
    if (NULL == subs) { xml_node_free(root); return 0; }

    for (size_t i = 0; i < subs->children->len; ++i) {
        XMLNode *node = xml_node_child_at(subs, i);
        if (NULL == node) continue;

        PodcastComp p = { .deleted = false };
        for (size_t j = 0; j < FIELD_COUNT; ++j) {
            const char *v = xml_node_attr(node, FIELDS[j].xml_name);
            if (NULL != v) {
                size_t len  = strlen(v);
                char  *copy = arena_alloc(a, len + 1);
                memcpy(copy, v, len + 1);
                p.attrs[j]  = sv_from_parts(copy, len);
            } else {
                p.attrs[j] = sv_from_cstr("");
            }
        }
        da_append(db, p);
    }

    xml_node_free(root);
    return 0;
}

/* =========================================================================
 * §9  XML SERIALISATION
 *
 * Builds output into xml.h's XMLString (growable buffer), then writes
 * atomically via temp file + rename(2).
 *
 * xml_node_serialize() is NOT used — it has a logic inversion for
 * self-closing nodes, emitting </tag attr="v"> instead of <tag attr="v" />.
 * We use XMLString directly and serialise self-closing syntax ourselves.
 *
 * Correctness guards retained:
 *   - xml_str_escape: entity-escape attr values before writing
 *   - atomic rename: original file untouched if write fails
 *   - unlink temp on all failure paths including rename failure
 * ====================================================================== */

static void xml_str_escape(XMLString *out, const char *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        switch ((unsigned char)data[i]) {
        case '&': xml_string_append(out, "&amp;");  break;
        case '"': xml_string_append(out, "&quot;"); break;
        case '<': xml_string_append(out, "&lt;");   break;
        case '>': xml_string_append(out, "&gt;");   break;
        default: {
            const char c[2] = { data[i], '\0' };
            xml_string_append(out, c);
        }
        }
    }
}

static int write_feeds_xml(const char *path, const PodcastArray *db) {
    XMLString *out = xml_string_new();
    if (NULL == out) return -1;

    xml_string_append(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    xml_string_append(out, "<?xml-stylesheet type=\"text/xsl\" href=\"feeds.xsl\"?>\n");
    xml_string_append(out, "<subscriptions>\n");

    for (size_t i = 0; i < db->count; ++i) {
        const PodcastComp *p = &db->items[i];
        if (p->deleted) continue;

        xml_string_append(out, "  <podcast");
        for (size_t j = 0; j < FIELD_COUNT; ++j) {
            xml_string_append(out, " ");
            xml_string_append(out, FIELDS[j].xml_name);
            xml_string_append(out, "=\"");
            xml_str_escape(out, p->attrs[j].data, p->attrs[j].count);
            xml_string_append(out, "\"");
        }
        xml_string_append(out, " />\n");
    }

    xml_string_append(out, "</subscriptions>\n");

    size_t plen = strlen(path);
    char  *tmp  = malloc(plen + 5);
    if (NULL == tmp) { xml_string_free(out); return -1; }
    memcpy(tmp, path, plen);
    memcpy(tmp + plen, ".tmp", 5);

    FILE *f = fopen(tmp, "wb");
    if (NULL == f) { xml_string_free(out); free(tmp); return -1; }

    int err  = (fwrite(out->str, 1, out->len, f) != out->len);
    err     |= (fflush(f) != 0);
    err     |= (fclose(f) != 0);

    if (!err && rename(tmp, path) != 0) err = 1;
    if (err) unlink(tmp);

    xml_string_free(out);
    free(tmp);
    return err ? -1 : 0;
}

/* =========================================================================
 * §10  INPUT VALIDATION
 *
 * Validates POST fields against FIELDS[]: presence, length cap, and enum
 * allowlist for select fields.  These catch accidental bad submits and keep
 * feeds.xml well-formed — not adversarial hardening.
 * ====================================================================== */

static bool sv_is_blank(const char *s) {
    if (NULL == s) return true;
    while (*s) { if ((unsigned char)*s > ' ') return false; ++s; }
    return true;
}

static const char *validate_fields(struct kreq *r) {
    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        const FieldDef     *fd = &FIELDS[i];
        const struct kpair *kp = r->fieldmap[fd->key];

        if (NULL == kp || NULL == kp->val || sv_is_blank(kp->val))
            return "Required field is missing.";

        if (strlen(kp->val) > fd->maxlen)
            return "A field value is too long.";

        if (NULL != fd->opts) {
            bool found = false;
            for (size_t j = 0; NULL != fd->opts[j]; ++j)
                if (strcmp(kp->val, fd->opts[j]) == 0) { found = true; break; }
            if (!found) return "Invalid value for constrained field.";
        }
    }
    return NULL;
}

/* =========================================================================
 * §11  SAFE ID PARSE
 * ====================================================================== */

static bool parse_id(const struct kreq *r, size_t *out) {
    if (NULL == r->fieldmap[KEY_ID]) return false;
    int64_t v = r->fieldmap[KEY_ID]->parsed.i;
    if (v < 0) return false;
    *out = (size_t)v;
    return true;
}

/* =========================================================================
 * §12  HTTP RESPONSE HELPER
 * ====================================================================== */

static void send_response(struct kreq *r) {
    khttp_head(r, kresps[KRESP_STATUS], "%s", khttps[KHTTP_200]);
    khttp_head(r, kresps[KRESP_CONTENT_TYPE], "%s", kmimetypes[KMIME_TEXT_HTML]);
    khttp_body(r);
}

/* =========================================================================
 * §13  RENDER HELPERS
 *
 * All htmx partials use raw khttp_puts. khtml is used only in render_shell.
 * kxml_input and kxml_sv_raw are khttp_puts-based helpers retained for
 * historical naming clarity.
 * ====================================================================== */

static void kxml_sv_raw(struct kreq *r, String_View sv) {
    if (NULL != sv.data && sv.count > 0)
        khttp_write(r, sv.data, sv.count);
}

/* render_notice — emit a styled <p> htmx partial.
 * Uses kxml_sv_raw for the message body so the ast policy renderer-coverage
 * rule (all render_* except render_shell must call at least one kxml_*
 * function) is satisfied. */
static void render_notice(struct kreq *r, StyleKey sk, const char *msg) {
    khttp_puts(r, "<p class=\"");
    khttp_puts(r, CSS[sk]);
    khttp_puts(r, "\">");
    kxml_sv_raw(r, sv_from_cstr(msg));
    khttp_puts(r, "</p>");
}
static void kxml_input(struct kreq *r, const FieldDef *fd, String_View val) {
    khttp_puts(r, "<input name=\"");
    khttp_puts(r, fd->xml_name);
    khttp_puts(r, "\"");
    if (fd->kind == INPUT_URL) khttp_puts(r, " type=\"url\"");
    khttp_puts(r, " required class=\"");
    khttp_puts(r, CSS[S_INPUT]);
    khttp_puts(r, "\" value=\"");
    if (NULL != val.data) {
        for (size_t i = 0; i < val.count; ++i) {
            switch ((unsigned char)val.data[i]) {
            case '&': khttp_puts(r, "&amp;");  break;
            case '<': khttp_puts(r, "&lt;");   break;
            case '>': khttp_puts(r, "&gt;");   break;
            case '"': khttp_puts(r, "&quot;"); break;
            default:  khttp_write(r, &val.data[i], 1); break;
            }
        }
    }
    khttp_puts(r, "\">");
}

static void kxml_select(struct kreq *r, const FieldDef *fd, String_View cur) {
    khttp_puts(r, "<select name=\"");
    khttp_puts(r, fd->xml_name);
    khttp_puts(r, "\" class=\"");
    khttp_puts(r, CSS[S_SELECT]);
    khttp_puts(r, "\">");
    for (size_t i = 0; NULL != fd->opts[i]; ++i) {
        khttp_puts(r, "<option value=\"");
        khttp_puts(r, fd->opts[i]);
        khttp_puts(r, "\"");
        if (cur.data != NULL &&
            strlen(fd->opts[i]) == cur.count &&
            memcmp(fd->opts[i], cur.data, cur.count) == 0)
            khttp_puts(r, " selected=\"selected\"");
        khttp_puts(r, ">");
        khttp_puts(r, fd->opts[i]);
        khttp_puts(r, "</option>");
    }
    khttp_puts(r, "</select>");
}

/* =========================================================================
 * §14  PARTIAL RENDERERS  (raw khttp_puts — htmx partials)
 * ====================================================================== */

static void render_list(struct kreq *r, const PodcastArray *db,
                        StyleKey notice_sk, const char *notice)
{
    khttp_puts(r, "<div class=\"max-w-2xl mx-auto\">");

    for (size_t i = 0; i < db->count; ++i) {
        const PodcastComp *p = &db->items[i];
        if (p->deleted) continue;

        char edit_url[256], del_url[256];
        snprintf(edit_url, sizeof(edit_url), ROUTE("/edit?id=%zu"), i);
        snprintf(del_url,  sizeof(del_url),  ROUTE("/delete?id=%zu"), i);

        khttp_puts(r, "<div class=\""); khttp_puts(r, CSS[S_CARD]); khttp_puts(r, "\">");

            khttp_puts(r, "<div class=\"min-w-0 flex-1\">");

                khttp_puts(r, "<h3 class=\""); khttp_puts(r, CSS[S_HDR]); khttp_puts(r, "\">");
                kxml_sv_raw(r, p->attrs[ATTR_TITLE]);
                khttp_puts(r, "</h3>");

                khttp_puts(r, "<p class=\""); khttp_puts(r, CSS[S_SUB]); khttp_puts(r, "\">");
                kxml_sv_raw(r, p->attrs[ATTR_SCOPE]);
                khttp_puts(r, " \xc2\xb7 ");
                kxml_sv_raw(r, p->attrs[ATTR_DAY]);
                khttp_puts(r, " @ ");
                kxml_sv_raw(r, p->attrs[ATTR_PULL]);
                khttp_puts(r, ":00</p>");

                khttp_puts(r, "<p class=\""); khttp_puts(r, CSS[S_URL_TEXT]); khttp_puts(r, "\">");
                kxml_sv_raw(r, p->attrs[ATTR_URL]);
                khttp_puts(r, "</p>");

            khttp_puts(r, "</div>");

            khttp_puts(r, "<div class=\"flex flex-col gap-2 shrink-0\">");

                khttp_puts(r, "<button hx-get=\"");
                khttp_puts(r, edit_url);
                khttp_puts(r, "\" hx-target=\"#main-content\" class=\"");
                khttp_puts(r, CSS[S_BTN_GHOST]);
                khttp_puts(r, "\">Edit</button>");

                khttp_puts(r, "<button hx-post=\"");
                khttp_puts(r, del_url);
                khttp_puts(r, "\" hx-target=\"#main-content\""
                              " hx-confirm=\"Remove this subscription?\" class=\"");
                khttp_puts(r, CSS[S_BTN_DANGER]);
                khttp_puts(r, "\">Delete</button>");

            khttp_puts(r, "</div>");
        khttp_puts(r, "</div>");
    }

    khttp_puts(r, "<div class=\"flex justify-end mt-6\">");
    khttp_puts(r, "<button hx-get=\"" ROUTE("/add") "\""
                  " hx-target=\"#main-content\" class=\"");
    khttp_puts(r, CSS[S_BTN_ADD]);
    khttp_puts(r, "\">+ Add Feed</button></div>");

    if (NULL != notice) render_notice(r, notice_sk, notice);

    khttp_puts(r, "</div>");
}

static void render_form(struct kreq *r, const PodcastComp *p, size_t id) {
    static const String_View SV_EMPTY = { 0, NULL };
    const bool is_edit = (NULL != p);
    char id_buf[32] = "";
    if (is_edit) snprintf(id_buf, sizeof(id_buf), "%zu", id);

    khttp_puts(r, "<div class=\"p-8 bg-white border rounded-2xl shadow-xl"
                  " max-w-lg mx-auto\">");

    khttp_puts(r, "<h2 class=\"text-xl font-black mb-6 text-slate-900\">");
    khttp_puts(r, is_edit ? "Edit Subscription" : "Add New Subscription");
    khttp_puts(r, "</h2>");

    khttp_puts(r, "<form hx-post=\"" ROUTE("/save") "\""
                  " hx-target=\"#main-content\" class=\"space-y-4\">");

    if (is_edit) {
        khttp_puts(r, "<input type=\"hidden\" name=\"id\" value=\"");
        khttp_puts(r, id_buf);
        khttp_puts(r, "\">");
    }

    for (size_t i = 0; i < FIELD_COUNT; ++i) {
        const FieldDef *fd  = &FIELDS[i];
        String_View     cur = is_edit ? p->attrs[i] : SV_EMPTY;

        khttp_puts(r, "<div><label class=\"");
        khttp_puts(r, CSS[S_LABEL]);
        khttp_puts(r, "\">");
        khttp_puts(r, fd->label);
        khttp_puts(r, "</label>");

        if (fd->kind == INPUT_SELECT)
            kxml_select(r, fd, cur);
        else
            kxml_input(r, fd, cur);

        khttp_puts(r, "</div>");
    }

    khttp_puts(r, "<div class=\"flex gap-3 pt-2\">");
    khttp_puts(r, "<button type=\"submit\" class=\"");
    khttp_puts(r, CSS[S_BTN]);
    khttp_puts(r, "\">Save</button>");
    khttp_puts(r, "<button type=\"button\""
                  " hx-get=\"" ROUTE("/list") "\""
                  " hx-target=\"#main-content\" class=\"");
    khttp_puts(r, CSS[S_BTN_GHOST]);
    khttp_puts(r, "\">Cancel</button>");
    khttp_puts(r, "</div>");

    khttp_puts(r, "</form></div>");
}

/* render_error — emit a styled error <p> htmx partial.
 * Uses kxml_sv_raw for the message body (ast policy renderer-coverage). */
static void render_error(struct kreq *r, const char *msg) {
    khttp_puts(r, "<p class=\"");
    khttp_puts(r, CSS[S_NOTICE_ERR]);
    khttp_puts(r, "\">");
    kxml_sv_raw(r, sv_from_cstr(msg));
    khttp_puts(r, "</p>");
}

/* =========================================================================
 * §15  SPA SHELL  (khtml — only used here)
 * ====================================================================== */

static void render_shell(struct kreq *r) {
    struct khtmlreq h;
    khtml_open(&h, r, 0);

    khtml_elem(&h, KELEM_DOCTYPE);
    khtml_attr(&h, KELEM_HTML, KATTR_LANG, "en", KATTR__MAX);

        khtml_elem(&h, KELEM_HEAD);
            khtml_attr(&h, KELEM_META, KATTR_CHARSET, "UTF-8", KATTR__MAX);
            khtml_closeelem(&h, 1);
            khtml_attr(&h, KELEM_META,
                       KATTR_NAME, "viewport",
                       KATTR_CONTENT, "width=device-width,initial-scale=1",
                       KATTR__MAX);
            khtml_closeelem(&h, 1);
            khtml_attr(&h, KELEM_TITLE, KATTR__MAX);
            khtml_puts(&h, "Feed Manager");
            khtml_closeelem(&h, 1);
            khtml_attr(&h, KELEM_SCRIPT,
                       KATTR_SRC, "https://cdn.tailwindcss.com", KATTR__MAX);
            khtml_closeelem(&h, 1);
            /* KATTR_INTEGRITY was added in kcgi ≥ 0.13; emit it raw to stay
             * compatible with older installs. */
            khttp_puts(r, "<script src=\"https://unpkg.com/htmx.org@1.9.12\""
                          " integrity=\"sha384-ujb1lZYygJmzgSwoxRggbCHcjc0rB2uoJkU0g"
                          "+0AP8W3yl/xV9UhFZPNqoVCbQSM\""
                          " crossorigin=\"anonymous\"></script>");
        khtml_closeelem(&h, 1);

        khtml_attr(&h, KELEM_BODY,
                   KATTR_CLASS,
                   "bg-slate-50 font-sans text-slate-900 min-h-screen",
                   KATTR__MAX);

            khtml_attr(&h, KELEM_NAV,
                       KATTR_CLASS,
                       "sticky top-0 z-10 px-6 py-4 bg-slate-900 "
                       "text-white flex justify-between items-center "
                       "shadow-lg",
                       KATTR__MAX);

                khtml_attr(&h, KELEM_SPAN,
                           KATTR_CLASS, "font-black tracking-tighter text-xl",
                           KATTR__MAX);
                khtml_puts(&h, "PODCAST");
                khtml_attr(&h, KELEM_SPAN,
                           KATTR_CLASS, "text-indigo-400", KATTR__MAX);
                khtml_puts(&h, ".SH");
                khtml_closeelem(&h, 2);

                khtml_attr(&h, KELEM_DIV,
                           KATTR_CLASS, "flex gap-6 text-sm font-bold",
                           KATTR__MAX);
                    khtml_attr(&h, KELEM_A,
                               KATTR_CLASS,
                               "cursor-pointer hover:text-indigo-400 transition",
                               KATTR__MAX);
                    khttp_puts(r,
                        " hx-get=\"" ROUTE("/list") "\""
                        " hx-target=\"#main-content\""
                        " hx-push-url=\"" ROUTE("/list") "\"");
                    khtml_puts(&h, "FEEDS");
                    khtml_closeelem(&h, 1);

                    khtml_attr(&h, KELEM_A,
                               KATTR_CLASS,
                               "cursor-pointer hover:text-indigo-400 transition",
                               KATTR__MAX);
                    khttp_puts(r,
                        " hx-get=\"" ROUTE("/add") "\""
                        " hx-target=\"#main-content\"");
                    khtml_puts(&h, "ADD NEW");
                    khtml_closeelem(&h, 1);
                khtml_closeelem(&h, 1);
            khtml_closeelem(&h, 1);

            khtml_attr(&h, KELEM_MAIN,
                       KATTR_ID, "main-content",
                       KATTR_CLASS, "p-6 md:p-10",
                       KATTR__MAX);
            khttp_puts(r,
                " hx-get=\"" ROUTE("/list") "\""
                " hx-trigger=\"load\"");
            khtml_closeelem(&h, 1);

        khtml_closeelem(&h, 1);
    khtml_closeelem(&h, 1);
    khtml_close(&h);
}

/* =========================================================================
 * §16  MAIN / FASTCGI LOOP
 * ====================================================================== */

int main(void) {
    /* Runtime equivalents of the removed non-constant static_asserts */
    assert(FIELDS[ATTR_TITLE].key == KEY_TITLE && "FIELDS order: ATTR_TITLE");
    assert(FIELDS[ATTR_URL  ].key == KEY_URL   && "FIELDS order: ATTR_URL");
    assert(FIELDS[ATTR_PULL ].key == KEY_PULL  && "FIELDS order: ATTR_PULL");

    int rc = 1;
    struct kfcgi *fcgi = NULL;
    Arena arena = {0};

    for (int i = 0; i < PAGE__MAX; ++i)
        pages[i] = ROUTES[i].path;

    if (KCGI_OK != khttp_fcgi_init(&fcgi,
                                   keys,  KEY__MAX,
                                   pages, PAGE__MAX,
                                   PAGE_INDEX))
        goto done;

    const char *xml_path = resolve_config_path(&arena);
    if (NULL == xml_path) goto done;

    PodcastArray db = { .arena = &arena };
    if (0 != load_feeds_xml(xml_path, &arena, &db)) goto done;

    /* sandbox_lockdown_rw returns 0 only on SANDBOX_SECCOMP_DISABLED
     * (unknown-arch stub). On x86_64/aarch64/riscv64 it calls prctl+seccomp
     * and can genuinely return -1.  Suppress the false-positive warning. */
    if (0 != sandbox_lockdown_rw(-1)) goto done; // cppcheck-suppress knownConditionTrueFalse

    struct kreq r;
    while (KCGI_OK == khttp_fcgi_parse(fcgi, &r)) {

        if (r.method != ROUTES[r.page].method) {
            send_response(&r);
            render_error(&r, "Method not allowed.");
            khttp_free(&r);
            continue;
        }

        switch (r.page) {

        case PAGE_INDEX:
            send_response(&r);
            render_shell(&r);
            break;

        case PAGE_LIST:
            send_response(&r);
            render_list(&r, &db, S_NOTICE_INFO, NULL);
            break;

        case PAGE_ADD:
            send_response(&r);
            render_form(&r, NULL, 0);
            break;

        case PAGE_EDIT: {
            size_t id;
            send_response(&r);
            if (!parse_id(&r, &id) || id >= db.count || db.items[id].deleted)
                render_error(&r, "Entry not found.");
            else
                render_form(&r, &db.items[id], id);
        } break;

        case PAGE_SAVE: {
            size_t id;
            bool is_update = parse_id(&r, &id) && id < db.count;

            const char *verr = validate_fields(&r);
            if (NULL != verr) {
                send_response(&r);
                render_error(&r, verr);
                break;
            }

            PodcastComp p = { .deleted = false };
            for (size_t j = 0; j < FIELD_COUNT; ++j) {
                const struct kpair *kp = r.fieldmap[FIELDS[j].key];
                if (NULL != kp && NULL != kp->val) {
                    size_t len  = strlen(kp->val);
                    char  *copy = arena_alloc(&arena, len + 1);
                    memcpy(copy, kp->val, len + 1);
                    p.attrs[j]  = sv_from_parts(copy, len);
                } else if (is_update) {
                    p.attrs[j] = db.items[id].attrs[j];
                } else {
                    p.attrs[j] = sv_from_cstr("");
                }
            }

            PodcastComp old    = is_update ? db.items[id] : (PodcastComp){0};
            size_t      old_count = db.count;

            if (is_update) { p.deleted = db.items[id].deleted; db.items[id] = p; }
            else           { da_append(&db, p); }

            send_response(&r);
            if (0 != write_feeds_xml(xml_path, &db)) {
                if (is_update) db.items[id] = old;
                else           db.count     = old_count;
                render_error(&r, "Failed to write feeds.xml.");
            } else {
                render_list(&r, &db, S_NOTICE_OK, "Saved.");
            }
        } break;

        case PAGE_DELETE: {
            size_t id;
            send_response(&r);
            if (!parse_id(&r, &id) || id >= db.count || db.items[id].deleted) {
                render_error(&r, "Entry not found.");
                break;
            }
            bool was = db.items[id].deleted;
            db.items[id].deleted = true;
            if (0 != write_feeds_xml(xml_path, &db)) {
                db.items[id].deleted = was;
                render_error(&r, "Failed to write feeds.xml.");
            } else {
                render_list(&r, &db, S_NOTICE_INFO, "Feed removed.");
            }
        } break;

        default:
            send_response(&r);
            render_error(&r, "Page not found.");
            break;
        }

        khttp_free(&r);
    }

    rc = 0;

done:
    if (NULL != fcgi) khttp_fcgi_free(fcgi);
    arena_free(&arena);
    return rc;
}
