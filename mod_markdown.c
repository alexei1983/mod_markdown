#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_log.h"
#include "ap_provider.h"
#include "apr_file_io.h"
#include "apr_strings.h"
#include "util_script.h"
#include "mod_markdown.h"
#include <cmark-gfm.h>
#include <cmark-gfm-core-extensions.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MOD_MARKDOWN_VERSION "1.1.1"
#define MARKDOWN_DEFAULT_MAX_SIZE (2 * 1024 * 1024)

enum {
    CFG_ENABLED       = 1u << 0,
    CFG_FULL_DOCUMENT = 1u << 1,
    CFG_RAW_LINK      = 1u << 2,
    CFG_TITLE_H1      = 1u << 3,
    CFG_MAX_SIZE      = 1u << 4,
    CFG_STYLESHEET    = 1u << 5,
    CFG_PREPROCESSOR  = 1u << 6,
    CFG_RAW_PARAMETER = 1u << 7
};

typedef struct {
    unsigned int set;
    int enabled;
    int full_document;
    int raw_link;
    int title_from_h1;
    apr_off_t max_size;
    const char *stylesheet;
    const char *preprocessor;
    const char *raw_parameter;
} markdown_cfg;

module AP_MODULE_DECLARE_DATA markdown_module;

static const char *html_escape(apr_pool_t *pool, const char *value)
{
    const char *src;
    apr_size_t size = 0;
    char *result;
    char *dst;

    if (!value) {
        return "";
    }

    for (src = value; *src; ++src) {
        size += *src == '&' ? 5 : (*src == '<' || *src == '>' ? 4 : (*src == '"' ? 6 : 1));
    }

    result = apr_palloc(pool, size + 1);
    dst = result;
    for (src = value; *src; ++src) {
        if (*src == '&') {
            memcpy(dst, "&amp;", 5); dst += 5;
        } else if (*src == '<') {
            memcpy(dst, "&lt;", 4); dst += 4;
        } else if (*src == '>') {
            memcpy(dst, "&gt;", 4); dst += 4;
        } else if (*src == '"') {
            memcpy(dst, "&quot;", 6); dst += 6;
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
    return result;
}

static apr_status_t render_impl(request_rec *r,
                                const char *markdown,
                                apr_size_t markdown_len,
                                const markdown_render_options *options,
                                const char **out,
                                apr_size_t *out_len)
{
    static const char *const extension_names[] = {
        "table", "strikethrough", "autolink", "tagfilter", "tasklist", NULL
    };
    cmark_parser *parser = NULL;
    cmark_node *document = NULL;
    cmark_llist *extensions;
    char *fragment = NULL;
    const char *result;
    int flags = CMARK_OPT_DEFAULT;
    int i;

    if (!r || !r->pool || !markdown || !out || !out_len) {
        return APR_EINVAL;
    }
    *out = NULL;
    *out_len = 0;

    parser = cmark_parser_new(flags);
    if (!parser) {
        return APR_ENOMEM;
    }

    for (i = 0; extension_names[i]; ++i) {
        cmark_syntax_extension *extension = cmark_find_syntax_extension(extension_names[i]);
        if (extension) {
            cmark_parser_attach_syntax_extension(parser, extension);
        }
    }

    cmark_parser_feed(parser, markdown, markdown_len);
    document = cmark_parser_finish(parser);
    if (!document) {
        cmark_parser_free(parser);
        return APR_EGENERAL;
    }

    extensions = cmark_parser_get_syntax_extensions(parser);
    fragment = cmark_render_html(document, flags, extensions);
    if (!fragment) {
        cmark_node_free(document);
        cmark_parser_free(parser);
        return APR_EGENERAL;
    }

    if (options && (options->flags & MARKDOWN_RENDER_FULL_DOCUMENT)) {
        const char *title = html_escape(r->pool,
                                        options->title ? options->title : "Markdown");
        const char *style = options->stylesheet
            ? apr_psprintf(r->pool,
                           "<link rel=\"stylesheet\" href=\"%s\">\n",
                           html_escape(r->pool, options->stylesheet))
            : "";
        const char *raw = options->raw_uri
            ? apr_psprintf(r->pool,
                           "<link rel=\"alternate\" type=\"text/markdown\" href=\"%s\">\n",
                           html_escape(r->pool, options->raw_uri))
            : "";

        result = apr_pstrcat(r->pool,
                             "<!doctype html>\n<html><head>",
                             "<meta charset=\"utf-8\">",
                             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n",
                             "<title>", title, "</title>\n",
                             style, raw,
                             "</head><body>\n", fragment,
                             "\n</body></html>\n", NULL);
    } else {
        result = apr_pstrdup(r->pool, fragment);
    }

    /* cmark_parser_new() uses cmark's default allocator; render_html's
     * returned buffer is documented as caller-owned and uses that allocator,
     * whose default implementation is libc malloc/free. */
    free(fragment);
    cmark_node_free(document);
    cmark_parser_free(parser);

    *out = result;
    *out_len = strlen(result);
    return APR_SUCCESS;
}

static markdown_renderer_provider_v1 renderer = {
    MARKDOWN_PROVIDER_ABI,
    sizeof(markdown_renderer_provider_v1),
    render_impl
};

static void *create_cfg(apr_pool_t *pool, char *directory)
{
    markdown_cfg *cfg = apr_pcalloc(pool, sizeof(*cfg));
    (void)directory;
    cfg->enabled = 1;
    cfg->full_document = 1;
    cfg->raw_link = 1;
    cfg->title_from_h1 = 1;
    cfg->max_size = MARKDOWN_DEFAULT_MAX_SIZE;
    cfg->raw_parameter = "raw";
    return cfg;
}

#define MERGE_FIELD(bit, field) merged->field = (add->set & bit) ? add->field : base->field
static void *merge_cfg(apr_pool_t *pool, void *basev, void *addv)
{
    const markdown_cfg *base = basev;
    const markdown_cfg *add = addv;
    markdown_cfg *merged = apr_pcalloc(pool, sizeof(*merged));

    merged->set = base->set | add->set;
    MERGE_FIELD(CFG_ENABLED, enabled);
    MERGE_FIELD(CFG_FULL_DOCUMENT, full_document);
    MERGE_FIELD(CFG_RAW_LINK, raw_link);
    MERGE_FIELD(CFG_TITLE_H1, title_from_h1);
    MERGE_FIELD(CFG_MAX_SIZE, max_size);
    MERGE_FIELD(CFG_STYLESHEET, stylesheet);
    MERGE_FIELD(CFG_PREPROCESSOR, preprocessor);
    MERGE_FIELD(CFG_RAW_PARAMETER, raw_parameter);
    return merged;
}
#undef MERGE_FIELD

static const char *set_flag(cmd_parms *cmd, void *value, int enabled)
{
    markdown_cfg *cfg = value;
    if (!strcasecmp(cmd->cmd->name, "MarkdownHtml")) {
        cfg->enabled = enabled; cfg->set |= CFG_ENABLED;
    } else if (!strcasecmp(cmd->cmd->name, "MarkdownFullDocument")) {
        cfg->full_document = enabled; cfg->set |= CFG_FULL_DOCUMENT;
    } else if (!strcasecmp(cmd->cmd->name, "MarkdownRawLink")) {
        cfg->raw_link = enabled; cfg->set |= CFG_RAW_LINK;
    } else {
        cfg->title_from_h1 = enabled; cfg->set |= CFG_TITLE_H1;
    }
    return NULL;
}

static const char *set_string(cmd_parms *cmd, void *value, const char *argument)
{
    markdown_cfg *cfg = value;
    if (!strcasecmp(cmd->cmd->name, "MarkdownStylesheet")) {
        cfg->stylesheet = argument; cfg->set |= CFG_STYLESHEET;
    } else if (!strcasecmp(cmd->cmd->name, "MarkdownRawParameter")) {
        cfg->raw_parameter = argument; cfg->set |= CFG_RAW_PARAMETER;
    } else {
        cfg->preprocessor = !strcasecmp(argument, "none") ? NULL : argument;
        cfg->set |= CFG_PREPROCESSOR;
    }
    return NULL;
}

static const char *set_size(cmd_parms *cmd, void *value, const char *argument)
{
    markdown_cfg *cfg = value;
    char *end = NULL;
    long parsed;
    (void)cmd;

    errno = 0;
    parsed = strtol(argument, &end, 10);
    if (errno == ERANGE || !end || *end || parsed < 1) {
        return "MarkdownMaxSize must be a positive integer";
    }
    cfg->max_size = (apr_off_t)parsed;
    cfg->set |= CFG_MAX_SIZE;
    return NULL;
}

static const command_rec commands[] = {
    AP_INIT_FLAG("MarkdownHtml", set_flag, NULL, OR_FILEINFO,
                 "Enable Markdown HTML rendering"),
    AP_INIT_FLAG("MarkdownFullDocument", set_flag, NULL, OR_FILEINFO,
                 "Render a complete HTML document"),
    AP_INIT_FLAG("MarkdownRawLink", set_flag, NULL, OR_FILEINFO,
                 "Include a raw Markdown link"),
    AP_INIT_FLAG("MarkdownTitleFromH1", set_flag, NULL, OR_FILEINFO,
                 "Use the first H1 as the page title"),
    AP_INIT_TAKE1("MarkdownStylesheet", set_string, NULL, OR_FILEINFO,
                  "Stylesheet URL"),
    AP_INIT_TAKE1("MarkdownRawParameter", set_string, NULL, OR_FILEINFO,
                  "Query parameter that requests raw Markdown"),
    AP_INIT_TAKE1("MarkdownPreprocessor", set_string, NULL, OR_FILEINFO,
                  "Preprocessor provider name or none"),
    AP_INIT_TAKE1("MarkdownMaxSize", set_size, NULL, OR_FILEINFO,
                  "Maximum Markdown file size"),
    { NULL }
};

static int accepts_html(request_rec *r)
{
    const char *accept = apr_table_get(r->headers_in, "Accept");
    return accept && strstr(accept, "text/html");
}

static const char *first_h1(apr_pool_t *pool, const char *input, apr_size_t length)
{
    const char *end = input + length;
    const char *line = input;

    while (line < end) {
        const char *line_end = memchr(line, '\n', (apr_size_t)(end - line));
        if (!line_end) {
            line_end = end;
        }
        if (line_end - line > 2 && line[0] == '#' && line[1] == ' ') {
            const char *title_end = line_end;
            if (title_end > line + 2 && title_end[-1] == '\r') {
                --title_end;
            }
            return apr_pstrndup(pool, line + 2, (apr_size_t)(title_end - line - 2));
        }
        line = line_end < end ? line_end + 1 : end;
    }
    return NULL;
}

static int write_bytes(request_rec *r, const char *data, apr_size_t length)
{
    while (length) {
        int chunk = length > (apr_size_t)INT_MAX ? INT_MAX : (int)length;
        int written = ap_rwrite(data, chunk, r);
        if (written <= 0) {
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        data += written;
        length -= (apr_size_t)written;
    }
    return OK;
}

static int markdown_handler(request_rec *r)
{
    markdown_cfg *cfg;
    apr_file_t *file = NULL;
    apr_finfo_t info;
    apr_table_t *args = NULL;
    char *buffer;
    apr_size_t size;
    apr_size_t sent = 0;
    const char *input;
    const char *html;
    const char *title;
    const char *raw_uri;
    apr_size_t input_len;
    apr_size_t html_len;
    markdown_render_options options;
    const markdown_preprocessor_provider_v1 *preprocessor = NULL;
    int condition;
    int raw = 0;

    if (!r || !r->per_dir_config) {
        return DECLINED;
    }
    cfg = ap_get_module_config(r->per_dir_config, &markdown_module);

    /* r->handler is NULL for most requests. The missing check here in 1.1.0
     * was the primary production SIGSEGV. */
    if (!cfg || !cfg->enabled || !r->handler || strcmp(r->handler, "markdown")) {
        return DECLINED;
    }
    if (!r->method || (strcmp(r->method, "GET") && strcmp(r->method, "HEAD"))) {
        return HTTP_METHOD_NOT_ALLOWED;
    }
    if (!r->filename ||
        apr_stat(&info, r->filename, APR_FINFO_MIN, r->pool) != APR_SUCCESS ||
        info.filetype != APR_REG) {
        return HTTP_NOT_FOUND;
    }
    if (info.size < 0 || info.size > cfg->max_size) {
        return HTTP_REQUEST_ENTITY_TOO_LARGE;
    }
    if (apr_file_open(&file, r->filename, APR_READ | APR_BINARY,
                      APR_OS_DEFAULT, r->pool) != APR_SUCCESS) {
        return HTTP_NOT_FOUND;
    }

    r->finfo = info;
    r->mtime = info.mtime;
    ap_set_last_modified(r);
    ap_set_etag(r);
    apr_table_merge(r->headers_out, "Vary", "Accept");
    condition = ap_meets_conditions(r);
    if (condition != OK) {
        apr_file_close(file);
        return condition;
    }

    if (cfg->raw_parameter && r->args) {
        ap_args_to_table(r, &args);
        raw = args && apr_table_get(args, cfg->raw_parameter) != NULL;
    }
    if (raw || !accepts_html(r)) {
        ap_set_content_type(r, "text/markdown; charset=utf-8");
        apr_table_setn(r->headers_out, "Link",
                       apr_psprintf(r->pool,
                                    "<%s>; rel=\"alternate\"; type=\"text/html\"",
                                    r->uri ? r->uri : ""));
        if (strcmp(r->method, "HEAD")) {
            ap_send_fd(file, r, 0, (apr_size_t)info.size, &sent);
        }
        apr_file_close(file);
        return OK;
    }

    size = (apr_size_t)info.size;
    buffer = apr_palloc(r->pool, size + 1);
    if (apr_file_read_full(file, buffer, size, NULL) != APR_SUCCESS) {
        apr_file_close(file);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    apr_file_close(file);
    buffer[size] = '\0';
    input = buffer;
    input_len = size;

    if (cfg->preprocessor) {
        preprocessor = ap_lookup_provider(MARKDOWN_PREPROCESSOR_PROVIDER_GROUP,
                                          cfg->preprocessor,
                                          MARKDOWN_PROVIDER_VERSION);
        if (!preprocessor || preprocessor->abi_version != MARKDOWN_PROVIDER_ABI ||
            preprocessor->struct_size < sizeof(*preprocessor) ||
            !preprocessor->process) {
            ap_log_rerror(APLOG_MARK, APLOG_ERR, 0, r,
                          "markdown: preprocessor provider '%s' is unavailable or incompatible",
                          cfg->preprocessor);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        if (preprocessor->process(r, input, input_len, &input, &input_len) != APR_SUCCESS ||
            !input) {
            return HTTP_BAD_REQUEST;
        }
    }

    title = cfg->title_from_h1 ? first_h1(r->pool, input, input_len) : NULL;
    if (!title) {
        title = r->filename;
    }
    raw_uri = cfg->raw_link && r->uri && cfg->raw_parameter
        ? apr_psprintf(r->pool, "%s?%s=1", r->uri, cfg->raw_parameter)
        : NULL;

    memset(&options, 0, sizeof(options));
    if (cfg->full_document) {
        options.flags |= MARKDOWN_RENDER_FULL_DOCUMENT;
    }
    options.title = title;
    options.stylesheet = cfg->stylesheet;
    options.raw_uri = raw_uri;

    if (render_impl(r, input, input_len, &options, &html, &html_len) != APR_SUCCESS ||
        !html) {
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    ap_set_content_type(r, "text/html; charset=utf-8");
    apr_table_setn(r->headers_out, "Link",
                   apr_psprintf(r->pool,
                                "<%s>; rel=\"alternate\"; type=\"text/markdown\"",
                                raw_uri ? raw_uri : (r->uri ? r->uri : "")));
    return !strcmp(r->method, "HEAD") ? OK : write_bytes(r, html, html_len);
}

static void register_hooks(apr_pool_t *pool)
{
    /* Register the global cmark-gfm extension set once while Apache is still
     * single-threaded, rather than racing on the first request. */
    cmark_gfm_core_extensions_ensure_registered();
    ap_register_provider(pool, MARKDOWN_RENDERER_PROVIDER_GROUP,
                         "cmark-gfm", MARKDOWN_PROVIDER_VERSION, &renderer);
    ap_hook_handler(markdown_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA markdown_module = {
    STANDARD20_MODULE_STUFF,
    create_cfg,
    merge_cfg,
    NULL,
    NULL,
    commands,
    register_hooks
};
