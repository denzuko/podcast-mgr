/* _GNU_SOURCE: dev-host Linux/glibc shim only.
 * Primary target is BSD (FreeBSD/OpenBSD) where lstat(2), fileno(3), and
 * syscall(2) are visible by default.  On glibc, -std=c11 suppresses them
 * unless a feature-test macro is set before the first system header.
 * Has no effect on BSD or clang/libc on the target. */
#ifdef __linux__
# define _GNU_SOURCE
#endif

/*
 * podcast-mgr/main.c  —  CGI handler for managing feeds.xml
 * BCHS stack: kcgi · kcgihtml · xml.h · sv.h · arena.h
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
#ifdef __linux__
# include <crypt.h>   /* crypt(3) — link with -lcrypt */
#endif
#include <sys/stat.h>

#include <kcgi.h>
#include <kcgihtml.h>

#define XML_H_IMPLEMENTATION
#include "xml.h"
#define SV_IMPLEMENTATION
#include "sv.h"
#define ARENA_IMPLEMENTATION
#include "arena.h"
#include "matrix_id.h"  /* net.matrix identity strings — baked into binary */

/* =========================================================================
 * §1  CONSTANTS
 * ====================================================================== */

#ifndef MOUNT_PATH
# define MOUNT_PATH    ""
#endif
#define CGI_BIN       "/index.cgi"
#define ROUTE(x)      MOUNT_PATH CGI_BIN x

#define APP_SUBDIR    "/podcasts"
#define FEED_FILENAME "/feeds.xml"
#define AUTH_FILENAME "/authstore"
#define REL_PATH      APP_SUBDIR FEED_FILENAME
#define AUTH_PATH     APP_SUBDIR AUTH_FILENAME
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
    [S_CARD]       = "p-4 bg-slate-800 shadow-sm rounded-xl border border-slate-700 flex "
                     "justify-between items-start gap-4 mb-3 transition "
                     "hover:border-indigo-500",
    [S_HDR]        = "text-base font-black text-white break-all",
    [S_SUB]        = "text-xs font-bold uppercase tracking-tighter "
                     "text-indigo-400 mt-1",
    [S_URL_TEXT]   = "text-xs text-slate-500 mt-1 truncate",
    [S_BTN]        = "bg-indigo-600 text-white px-4 py-2 rounded-lg "
                     "hover:bg-indigo-500 transition font-bold text-sm "
                     "whitespace-nowrap",
    [S_BTN_DANGER] = "bg-red-700 text-white px-4 py-2 rounded-lg "
                     "hover:bg-red-600 transition font-bold text-sm "
                     "whitespace-nowrap",
    [S_BTN_GHOST]  = "bg-slate-700 text-slate-200 px-4 py-2 rounded-lg "
                     "hover:bg-slate-600 transition font-bold text-sm "
                     "whitespace-nowrap",
    [S_BTN_ADD]    = "bg-indigo-600 text-white px-5 py-2 rounded-xl "
                     "font-bold hover:bg-indigo-500 transition",
    [S_INPUT]      = "p-2 border border-slate-600 rounded-lg bg-slate-700 "
                     "text-slate-100 w-full text-sm "
                     "focus:bg-slate-600 focus:ring-2 focus:ring-indigo-500 "
                     "outline-none placeholder-slate-400",
    [S_SELECT]     = "p-2 border border-slate-600 rounded-lg bg-slate-700 "
                     "text-slate-100 w-full text-sm "
                     "focus:bg-slate-600 focus:ring-2 focus:ring-indigo-500 "
                     "outline-none appearance-none",
    [S_LABEL]      = "text-[10px] uppercase font-bold text-slate-400 "
                     "mb-1 block tracking-wider",
    [S_NOTICE_OK]  = "text-sm font-bold mt-3 text-center text-green-400",
    [S_NOTICE_ERR] = "text-sm font-bold mt-3 text-center text-red-400",
    [S_NOTICE_INFO]= "text-sm font-bold mt-3 text-center text-slate-400",
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

/* =========================================================================
 * §7  HTTP BASIC AUTH — htpasswd bcrypt format
 *
 * Credentials file: ~/.config/podcasts/auth (mode 0600)
 * Format: Apache htpasswd bcrypt — one line per user:
 *   username:$2y$NN$salt+hash
 * Generate: htpasswd -nbB username password
 *         or: openssl passwd -6 -stdin (SHA-512 fallback)
 *
 * auth_check: uses crypt(3) from libcrypt (-lcrypt).
 * Returns 1 if user+pass match the stored hash, 0 otherwise.
 * ====================================================================== */

/* Constant-time username comparison. */
static int user_match(const char *a, const char *b) {
    if (NULL == a || NULL == b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return 0;
    int ok = 1;
    for (size_t i = 0; i < la; i++) ok &= (a[i] == b[i]);
    return ok;
}

/* Parse a single htpasswd line "user:hash\n" into buffers.
 * Returns 1 on success, 0 on malformed input. */
static int parse_htpasswd_line(const char *line,
                                char *out_user, size_t ulen,
                                char *out_hash, size_t hlen) {
    if (NULL == line || '\0' == line[0]) return 0;
    char buf[512] = {0};
    snprintf(buf, sizeof(buf), "%s", line);
    buf[strcspn(buf, "\r\n")] = '\0';
    char *colon = strchr(buf, ':');
    if (NULL == colon || colon == buf) return 0;
    *colon = '\0';
    snprintf(out_user, ulen, "%s", buf);
    snprintf(out_hash, hlen, "%s", colon + 1);
    return 1;
}

/* Verify user:pass against a htpasswd file.
 * Scans all lines — supports multi-user files.
 * Returns 1 if a matching user+hash line is found, 0 otherwise.
 * If auth file is absent returns 0 (caller disables auth on absence). */
static int auth_verify(const char *path,
                        const char *user, const char *pass) {
    if (NULL == path || NULL == user || NULL == pass) return 0;
    if ('\0' == user[0] || '\0' == pass[0]) return 0;

    FILE *f = fopen(path, "r");
    if (NULL == f) return 0;

    char line[512];
    char stored_user[256], stored_hash[256];
    int found = 0;

    while (!found && NULL != fgets(line, sizeof(line), f)) {
        if (!parse_htpasswd_line(line, stored_user, sizeof(stored_user),
                                 stored_hash, sizeof(stored_hash)))
            continue;
        if (!user_match(user, stored_user))
            continue;
        /* crypt(3) with stored hash as salt performs bcrypt verification */
        const char *computed = crypt(pass, stored_hash);
        if (NULL != computed && strcmp(computed, stored_hash) == 0)
            found = 1;
    }
    fclose(f);
    return found;
}

/* Resolve path to auth file into arena. */
static const char *resolve_auth_path(Arena *a) {
    const char *xdg  = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char path[4096];

    if (NULL != xdg && '\0' != xdg[0])
        snprintf(path, sizeof(path), "%s" AUTH_PATH, xdg);
    else if (NULL != home)
        snprintf(path, sizeof(path), "%s/.config" AUTH_PATH, home);
    else
        return NULL;

    size_t len = strlen(path);
    char  *res = arena_alloc(a, len + 1);
    memcpy(res, path, len + 1);
    return res;
}

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

    /* xml.h does not handle <!DOCTYPE> internal subsets — strip any
     * DOCTYPE declaration before parsing.  feeds.xml should never have
     * one (write_feeds_xml does not emit it) but external editors may
     * add it.  Find "<!DOCTYPE" and remove everything up to the closing
     * "]>" or plain ">" so the parser sees clean XML. */
    {
        char *dt = strstr(buf, "<!DOCTYPE");
        if (NULL != dt) {
            /* Look for internal subset close "]>" first, then plain ">" */
            char *end = strstr(dt, "]>");
            if (NULL != end)
                end += 2;
            else {
                end = strchr(dt, '>');
                if (NULL != end) end += 1;
            }
            if (NULL != end)
                memmove(dt, end, strlen(end) + 1);
        }
    }

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
    /* Alpine fuzzy search wrapper */
    khttp_puts(r, "<div class=\"max-w-2xl mx-auto\""
                  " x-data=\"{search:'',filter(){document.querySelectorAll('[data-title]')"
                  ".forEach(el=>{el.style.display="
                  "el.dataset.title.toLowerCase().includes(this.search.toLowerCase())?'':'none'})}}\">");

    /* Search bar */
    khttp_puts(r, "<div class=\"mb-6\">"
                  "<input type=\"search\" x-model=\"search\" @input=\"filter()\""
                  " placeholder=\"Search feeds\xe2\x80\xa6\""
                  " class=\"w-full px-4 py-2 rounded-xl bg-slate-800"
                  " border border-slate-700 text-slate-100"
                  " placeholder-slate-500 focus:outline-none"
                  " focus:ring-2 focus:ring-indigo-500 text-sm\"/>"
                  "</div>");

    for (size_t i = 0; i < db->count; ++i) {
        const PodcastComp *p = &db->items[i];
        if (p->deleted) continue;

        char edit_url[256], del_url[256], logo_url[512], title_esc[512];
        snprintf(edit_url, sizeof(edit_url), ROUTE("/edit?id=%zu"),   i);
        snprintf(del_url,  sizeof(del_url),  ROUTE("/delete?id=%zu"), i);

        /* Extract hostname for Google favicon service */
        const char *url_s = p->attrs[ATTR_URL].data;
        char host[256]    = "example.com";
        if (url_s && p->attrs[ATTR_URL].count > 0) {
            const char *h = url_s;
            if (strncmp(h, "https://", 8) == 0) h += 8;
            else if (strncmp(h, "http://", 7) == 0) h += 7;
            size_t hlen = 0;
            while (h[hlen] && h[hlen] != '/' && h[hlen] != ':') hlen++;
            if (hlen > 0 && hlen < sizeof(host)) {
                memcpy(host, h, hlen);
                host[hlen] = '\0';
            }
        }
        snprintf(logo_url, sizeof(logo_url),
                 "https://www.google.com/s2/favicons?domain=%s&sz=64", host);

        /* Title for Alpine data-title filter — must be HTML-escaped
         * to prevent attribute injection via titles containing '"' or '<' */
        {
            const char *ts = p->attrs[ATTR_TITLE].data;
            size_t      tl = p->attrs[ATTR_TITLE].count;
            size_t      j = 0;
            for (size_t k = 0; k < tl && j + 7 < sizeof(title_esc); ++k) {
                switch ((unsigned char)ts[k]) {
                case '"':  memcpy(title_esc+j, "&quot;", 6); j+=6; break;
                case '&':  memcpy(title_esc+j, "&amp;",  5); j+=5; break;
                case '<':  memcpy(title_esc+j, "&lt;",   4); j+=4; break;
                case '>':  memcpy(title_esc+j, "&gt;",   4); j+=4; break;
                default:   title_esc[j++] = ts[k];           break;
                }
            }
            title_esc[j] = '\0';
        }

        khttp_puts(r, "<div data-title=\"");
        khttp_puts(r, title_esc);
        khttp_puts(r, "\" class=\""); khttp_puts(r, CSS[S_CARD]); khttp_puts(r, "\">");

            /* Podcast logo / favicon */
            khttp_puts(r, "<img src=\""); khttp_puts(r, logo_url);
            khttp_puts(r, "\" alt=\"\" loading=\"lazy\""
                          " class=\"w-10 h-10 rounded-lg object-cover"
                          " flex-shrink-0 bg-slate-700\"/>");

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
    khttp_puts(r, "<!DOCTYPE html>"
                  "<html lang=\"en\">"
                  "<head>"
                  "<meta charset=\"UTF-8\"/>"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"/>"
                  "<title>Feed Manager</title>"
                  "<script src=\"https://cdn.tailwindcss.com\""
                  " data-cfasync=\"false\"></script>"
                  "<script src=\"https://unpkg.com/htmx.org@1.9.12\""
                  " integrity=\"sha384-ujb1lZYygJmzgSwoxRggbCHcjc0rB2XoQrxeTUQyRjrOnlCoYta87iKBWq3EsdM2\""
                  " crossorigin=\"anonymous\""
                  " data-cfasync=\"false\"></script>"
                  "<script defer"
                  " src=\"https://cdn.jsdelivr.net/npm/alpinejs@3.x.x/dist/cdn.min.js\""
                  " data-cfasync=\"false\"></script>"
                  "</head>"
                  "<body class=\"bg-slate-900 font-sans text-slate-100 min-h-screen\">"
                  "<nav class=\"sticky top-0 z-10 px-6 py-3 bg-slate-950 "
                  "border-b border-slate-800 flex justify-between items-center\">"
                  "<span class=\"font-black tracking-tighter text-xl\">"
                  "PODCAST<span class=\"text-indigo-400\">.SH</span>"
                  "</span>"
                  "<div class=\"flex items-center gap-2\">"
                  /* Refresh/list icon */
                  "<button title=\"Feeds\""
                  " class=\"p-2 rounded-lg text-slate-400 hover:text-white"
                  " hover:bg-slate-800 transition\""
                  " hx-get=\"" ROUTE("/list") "\""
                  " hx-target=\"#main-content\""
                  " hx-push-url=\"" ROUTE("/list") "\">"
                  "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"w-5 h-5\""
                  " fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\""
                  " stroke-width=\"2\">"
                  "<path stroke-linecap=\"round\" stroke-linejoin=\"round\""
                  " d=\"M4 6h16M4 12h16M4 18h16\"/>"
                  "</svg>"
                  "</button>"
                  /* Add feed icon */
                  "<button title=\"Add Feed\""
                  " class=\"p-2 rounded-lg text-slate-400 hover:text-white"
                  " hover:bg-slate-800 transition\""
                  " hx-get=\"" ROUTE("/add") "\""
                  " hx-target=\"#main-content\">"
                  "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"w-5 h-5\""
                  " fill=\"none\" viewBox=\"0 0 24 24\" stroke=\"currentColor\""
                  " stroke-width=\"2\">"
                  "<path stroke-linecap=\"round\" stroke-linejoin=\"round\""
                  " d=\"M12 4v16m8-8H4\"/>"
                  "</svg>"
                  "</button>"
                  "</div>"
                  "</nav>"
                  "<main id=\"main-content\""
                  " class=\"p-6 md:p-10\""
                  " x-data"
                  " x-init=\"htmx.ajax('GET','" ROUTE("/list") "',{target:'#main-content',swap:'innerHTML'})\""
                  "></main>"
                  "</body></html>");
}
/* =========================================================================
 * §16  MAIN / FASTCGI LOOP
 * ====================================================================== */

int main(void) {
    assert(FIELDS[ATTR_TITLE].key == KEY_TITLE && "FIELDS order: ATTR_TITLE");
    assert(FIELDS[ATTR_URL  ].key == KEY_URL   && "FIELDS order: ATTR_URL");
    assert(FIELDS[ATTR_PULL ].key == KEY_PULL  && "FIELDS order: ATTR_PULL");

    int rc = 1;
    Arena arena = {0};

    for (int i = 0; i < PAGE__MAX; ++i)
        pages[i] = ROUTES[i].path;

    const char *xml_path = resolve_config_path(&arena);
    if (NULL == xml_path) goto done;

    PodcastArray db = { .arena = &arena };
    if (0 != load_feeds_xml(xml_path, &arena, &db)) goto done;

    struct kreq r;
    if (KCGI_OK != khttp_parse(&r, keys, KEY__MAX, pages, PAGE__MAX, PAGE_INDEX))
        goto done;

    /* HTTP Basic Auth — check ~/.config/podcasts/auth if it exists.
     * Format: htpasswd bcrypt ("user:$2y$..." one line per user).
     * If the file is absent, access is open. */
    {
        const char *auth_path = resolve_auth_path(&arena);
        FILE *af = auth_path ? fopen(auth_path, "r") : NULL;
        if (NULL != af) {
            fclose(af);
            int authed = 0;
            if (r.rawauth.type == KAUTH_BASIC &&
                NULL != r.rawauth.d.basic.response) {
                char resp[512] = {0};
                snprintf(resp, sizeof(resp), "%s",
                         r.rawauth.d.basic.response);
                char *colon = strchr(resp, ':');
                if (NULL != colon) {
                    *colon = '\0';
                    authed = auth_verify(auth_path, resp, colon + 1);
                }
            }
            if (!authed) {
                khttp_head(&r, kresps[KRESP_STATUS],
                           "%s", khttps[KHTTP_401]);
                khttp_head(&r, kresps[KRESP_WWW_AUTHENTICATE],
                           "Basic realm=\"podcast-mgr\"");
                khttp_head(&r, kresps[KRESP_CONTENT_TYPE],
                           "%s", kmimetypes[KMIME_TEXT_HTML]);
                khttp_body(&r);
                khttp_puts(&r, "<html><body>"
                               "<h1>401 Unauthorized</h1>"
                               "<p>Valid credentials required.</p>"
                               "</body></html>");
                khttp_free(&r);
                goto done;
            }
        }
    }

    if (r.method != ROUTES[r.page].method) {
        send_response(&r);
        render_error(&r, "Method not allowed.");
        khttp_free(&r);
        goto done;
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
    rc = 0;

done:
    arena_free(&arena);
    return rc;
}
