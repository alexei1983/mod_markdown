/*
 * mod_markdown.c
 *
 * Apache 2.4+ module that serves Markdown files as either:
 *
 *   text/html
 *   text/markdown
 *
 * based on HTTP Accept negotiation.
 *
 * Features:
 *
 *   - Configurable Markdown extensions
 *   - .md enabled by default
 *   - HTML rendering via cmark-gfm
 *   - Raw Markdown via Accept: text/markdown
 *   - Explicit raw query parameter
 *   - Optional "View raw Markdown" link
 *   - Optional stylesheet
 *   - Optional title extraction from first H1
 *   - ETag
 *   - Last-Modified
 *   - Apache-native conditional request handling via ap_meets_conditions()
 *
 * Requires:
 *
 *   Apache 2.4+
 *   APR
 *   cmark-gfm
 */

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_request.h"
#include "http_log.h"
#include "http_core.h"

#include "apr_strings.h"
#include "apr_file_io.h"
#include "apr_lib.h"
#include "apr_tables.h"
#include "apr_time.h"
#include "apr_date.h"

#include <string.h>
#include <stdlib.h>

#include <cmark-gfm.h>


#define MOD_MARKDOWN_HANDLER "markdown-handler"

module AP_MODULE_DECLARE_DATA markdown_module;


/*
 * ==========================================================================
 * Configuration
 * ==========================================================================
 */

typedef struct
{
    apr_array_header_t *extensions;

    int enabled;
    int html_enabled;
    int raw_link_enabled;
    int title_from_h1;

    const char *stylesheet;
    const char *raw_parameter;

} markdown_dir_config;


static void *markdown_create_dir_config(
    apr_pool_t *pool,
    char *dir)
{
    markdown_dir_config *config =
        apr_pcalloc(
            pool,
            sizeof(markdown_dir_config));

    config->extensions =
        apr_array_make(
            pool,
            4,
            sizeof(const char *));

    /*
     * .md is enabled by default.
     */
    APR_ARRAY_PUSH(
        config->extensions,
        const char *) = ".md";

    config->enabled = 1;
    config->html_enabled = 1;
    config->raw_link_enabled = 1;
    config->title_from_h1 = 1;

    config->stylesheet = NULL;
    config->raw_parameter = "raw";

    return config;
}


/*
 * ==========================================================================
 * Configuration directives
 * ==========================================================================
 */

static const char *markdown_set_enabled(
    cmd_parms *cmd,
    void *cfg,
    int flag)
{
    markdown_dir_config *config =
        (markdown_dir_config *)cfg;

    config->enabled = flag;

    return NULL;
}


static const char *markdown_set_html_enabled(
    cmd_parms *cmd,
    void *cfg,
    int flag)
{
    markdown_dir_config *config =
        (markdown_dir_config *)cfg;

    config->html_enabled = flag;

    return NULL;
}


static const char *markdown_set_raw_link_enabled(
    cmd_parms *cmd,
    void *cfg,
    int flag)
{
    markdown_dir_config *config =
        (markdown_dir_config *)cfg;

    config->raw_link_enabled = flag;

    return NULL;
}


static const char *markdown_set_title_from_h1(
    cmd_parms *cmd,
    void *cfg,
    int flag)
{
    markdown_dir_config *config =
        (markdown_dir_config *)cfg;

    config->title_from_h1 = flag;

    return NULL;
}


static const char *markdown_set_stylesheet(
    cmd_parms *cmd,
    void *cfg,
    const char *value)
{
    markdown_dir_config *config =
        (markdown_dir_config *)cfg;

    config->stylesheet =
        apr_pstrdup(
            cmd->pool,
            value);

    return NULL;
}


static const char *markdown_set_raw_parameter(
    cmd_parms *cmd,
    void *cfg,
    const char *value)
{
    markdown_dir_config *config =
        (markdown_dir_config *)cfg;

    if (
        value == NULL ||
        *value == '\0')
    {
        return "MarkdownRawParameter requires a non-empty parameter name";
    }

    config->raw_parameter =
        apr_pstrdup(
            cmd->pool,
            value);

    return NULL;
}


static const char *markdown_add_extension(
    cmd_parms *cmd,
    void *cfg,
    const char *extension)
{
    markdown_dir_config *config =
        (markdown_dir_config *)cfg;

    if (
        extension == NULL ||
        *extension == '\0')
    {
        return "MarkdownExtension requires a file extension";
    }

    const char *normalized;

    if (extension[0] == '.')
    {
        normalized =
            apr_pstrdup(
                cmd->pool,
                extension);
    }
    else
    {
        normalized =
            apr_pstrcat(
                cmd->pool,
                ".",
                extension,
                NULL);
    }

    APR_ARRAY_PUSH(
        config->extensions,
        const char *) = normalized;

    return NULL;
}


/*
 * ==========================================================================
 * Extension matching
 * ==========================================================================
 */

static int markdown_has_supported_extension(
    request_rec *r,
    markdown_dir_config *config)
{
    if (
        r->filename == NULL ||
        config == NULL)
    {
        return 0;
    }

    const char **extensions =
        (const char **)config->extensions->elts;

    size_t filename_length =
        strlen(r->filename);

    int i;

    for (
        i = 0;
        i < config->extensions->nelts;
        i++)
    {
        const char *extension =
            extensions[i];

        size_t extension_length =
            strlen(extension);

        if (
            filename_length <
            extension_length)
        {
            continue;
        }

        const char *candidate =
            r->filename +
            filename_length -
            extension_length;

        if (
            strcasecmp(
                candidate,
                extension) == 0)
        {
            return 1;
        }
    }

    return 0;
}


/*
 * ==========================================================================
 * Query-string helpers
 * ==========================================================================
 */

static int markdown_raw_requested(
    request_rec *r,
    markdown_dir_config *config)
{
    if (
        r->args == NULL ||
        config == NULL ||
        config->raw_parameter == NULL)
    {
        return 0;
    }

    char *args =
        apr_pstrdup(
            r->pool,
            r->args);

    char *last = NULL;

    char *token =
        apr_strtok(
            args,
            "&",
            &last);

    while (token != NULL)
    {
        char *equals =
            strchr(
                token,
                '=');

        if (equals != NULL)
        {
            *equals = '\0';

            const char *name =
                token;

            const char *value =
                equals + 1;

            if (
                strcasecmp(
                    name,
                    config->raw_parameter) == 0)
            {
                if (
                    strcmp(value, "1") == 0 ||
                    strcasecmp(value, "true") == 0 ||
                    strcasecmp(value, "yes") == 0 ||
                    strcasecmp(value, "on") == 0)
                {
                    return 1;
                }
            }
        }

        token =
            apr_strtok(
                NULL,
                "&",
                &last);
    }

    return 0;
}


/*
 * ==========================================================================
 * Accept negotiation
 * ==========================================================================
 */

typedef struct
{
    double html_q;
    double markdown_q;

} markdown_accept_preferences;


static double markdown_parse_quality(
    apr_pool_t *pool,
    const char *parameters)
{
    if (
        parameters == NULL ||
        *parameters == '\0')
    {
        return 1.0;
    }

    char *copy =
        apr_pstrdup(
            pool,
            parameters);

    char *last = NULL;

    char *token =
        apr_strtok(
            copy,
            ";",
            &last);

    while (token != NULL)
    {
        while (
            *token &&
            apr_isspace(*token))
        {
            token++;
        }

        if (
            strncasecmp(
                token,
                "q=",
                2) == 0)
        {
            double q =
                atof(token + 2);

            if (q < 0.0)
            {
                q = 0.0;
            }

            if (q > 1.0)
            {
                q = 1.0;
            }

            return q;
        }

        token =
            apr_strtok(
                NULL,
                ";",
                &last);
    }

    return 1.0;
}


static markdown_accept_preferences markdown_parse_accept(
    request_rec *r)
{
    markdown_accept_preferences result;

    /*
     * Browser-friendly default if Accept is absent.
     */
    result.html_q = 1.0;
    result.markdown_q = 0.0;

    const char *accept =
        apr_table_get(
            r->headers_in,
            "Accept");

    if (
        accept == NULL ||
        *accept == '\0')
    {
        return result;
    }

    result.html_q = -1.0;
    result.markdown_q = -1.0;

    char *copy =
        apr_pstrdup(
            r->pool,
            accept);

    char *last = NULL;

    char *entry =
        apr_strtok(
            copy,
            ",",
            &last);

    while (entry != NULL)
    {
        while (
            *entry &&
            apr_isspace(*entry))
        {
            entry++;
        }

        char *semicolon =
            strchr(
                entry,
                ';');

        char *parameters = NULL;

        if (semicolon != NULL)
        {
            *semicolon = '\0';
            parameters = semicolon + 1;
        }

        char *end =
            entry +
            strlen(entry);

        while (
            end > entry &&
            apr_isspace(*(end - 1)))
        {
            end--;
        }

        *end = '\0';

        double q =
            markdown_parse_quality(
                r->pool,
                parameters);

        if (
            strcasecmp(
                entry,
                "text/html") == 0)
        {
            if (q > result.html_q)
            {
                result.html_q = q;
            }
        }
        else if (
            strcasecmp(
                entry,
                "text/markdown") == 0)
        {
            if (
                q >
                result.markdown_q)
            {
                result.markdown_q = q;
            }
        }
        else if (
            strcasecmp(
                entry,
                "text/*") == 0 ||
            strcmp(
                entry,
                "*/*") == 0)
        {
            if (q > result.html_q)
            {
                result.html_q = q;
            }

            if (
                q >
                result.markdown_q)
            {
                result.markdown_q = q;
            }
        }

        entry =
            apr_strtok(
                NULL,
                ",",
                &last);
    }

    if (result.html_q < 0.0)
    {
        result.html_q = 0.0;
    }

    if (
        result.markdown_q < 0.0)
    {
        result.markdown_q = 0.0;
    }

    return result;
}


/*
 * Return:
 *
 *   1 = HTML
 *   0 = raw Markdown
 *  -1 = neither acceptable
 */
static int markdown_wants_html(
    request_rec *r,
    markdown_dir_config *config)
{
    if (
        markdown_raw_requested(
            r,
            config))
    {
        return 0;
    }

    markdown_accept_preferences prefs =
        markdown_parse_accept(r);

    if (!config->html_enabled)
    {
        if (
            prefs.markdown_q <= 0.0)
        {
            return -1;
        }

        return 0;
    }

    if (
        prefs.html_q <= 0.0 &&
        prefs.markdown_q <= 0.0)
    {
        return -1;
    }

    /*
     * HTML wins equal preference.
     */
    if (
        prefs.html_q >=
        prefs.markdown_q)
    {
        return 1;
    }

    return 0;
}


/*
 * ==========================================================================
 * File metadata
 * ==========================================================================
 */

static apr_status_t markdown_get_file_info(
    request_rec *r,
    apr_finfo_t *info)
{
    return apr_stat(
        info,
        r->filename,
        APR_FINFO_SIZE |
        APR_FINFO_MTIME |
        APR_FINFO_TYPE,
        r->pool);
}


/*
 * ==========================================================================
 * ETag / Last-Modified
 * ==========================================================================
 */

/*
 * We deliberately create different ETags for the HTML and raw-Markdown
 * representations.
 *
 * Example:
 *
 *   "68d1b94f-1842-html"
 *   "68d1b94f-1842-md"
 *
 * This prevents validators for one representation from matching the other.
 */
static const char *markdown_make_etag(
    request_rec *r,
    const apr_finfo_t *info,
    int html_representation)
{
    return apr_psprintf(
        r->pool,
        "\"%" APR_TIME_T_FMT "-%" APR_OFF_T_FMT "-%s\"",
        info->mtime,
        info->size,
        html_representation
            ? "html"
            : "md");
}


/*
 * Set the metadata required by Apache's conditional-request machinery.
 */
static void markdown_set_cache_validators(
    request_rec *r,
    const apr_finfo_t *info,
    int html_representation)
{
    /*
     * Populate r->mtime.
     */
    ap_update_mtime(
        r,
        info->mtime);

    /*
     * Generate standard Last-Modified header from r->mtime.
     */
    ap_set_last_modified(r);

    /*
     * Representation-specific ETag.
     */
    const char *etag =
        markdown_make_etag(
            r,
            info,
            html_representation);

    apr_table_setn(
        r->headers_out,
        "ETag",
        etag);

    /*
     * Content representation changes according to Accept.
     */
    apr_table_mergen(
        r->headers_out,
        "Vary",
        "Accept");
}


/*
 * ==========================================================================
 * File reading
 * ==========================================================================
 */

static apr_status_t markdown_read_file(
    request_rec *r,
    const apr_finfo_t *info,
    char **buffer,
    apr_size_t *length)
{
    if (
        info->filetype !=
        APR_REG)
    {
        return APR_EGENERAL;
    }

    apr_file_t *file;

    apr_status_t status =
        apr_file_open(
            &file,
            r->filename,
            APR_READ |
            APR_BINARY,
            APR_OS_DEFAULT,
            r->pool);

    if (
        status !=
        APR_SUCCESS)
    {
        return status;
    }

    apr_size_t size =
        (apr_size_t)info->size;

    *buffer =
        apr_palloc(
            r->pool,
            size + 1);

    apr_size_t bytes_read =
        size;

    status =
        apr_file_read_full(
            file,
            *buffer,
            size,
            &bytes_read);

    apr_file_close(file);

    if (
        status != APR_SUCCESS &&
        status != APR_EOF)
    {
        return status;
    }

    (*buffer)[bytes_read] =
        '\0';

    *length =
        bytes_read;

    return APR_SUCCESS;
}


/*
 * ==========================================================================
 * HTML helpers
 * ==========================================================================
 */

static const char *markdown_escape_html(
    request_rec *r,
    const char *input)
{
    if (input == NULL)
    {
        return "";
    }

    return ap_escape_html(
        r->pool,
        input);
}


static const char *markdown_extract_h1(
    request_rec *r,
    const char *markdown)
{
    if (
        markdown == NULL ||
        *markdown == '\0')
    {
        return NULL;
    }

    const char *p =
        markdown;

    while (*p)
    {
        const char *line_start =
            p;

        const char *line_end =
            strchr(
                p,
                '\n');

        if (line_end == NULL)
        {
            line_end =
                p +
                strlen(p);
        }

        if (
            line_end - line_start >= 2 &&
            line_start[0] == '#' &&
            (
                line_start[1] == ' ' ||
                line_start[1] == '\t'
            ))
        {
            const char *title_start =
                line_start + 2;

            while (
                title_start < line_end &&
                apr_isspace(*title_start))
            {
                title_start++;
            }

            const char *title_end =
                line_end;

            while (
                title_end > title_start &&
                apr_isspace(*(title_end - 1)))
            {
                title_end--;
            }

            /*
             * Optional closing # characters.
             */
            while (
                title_end > title_start &&
                *(title_end - 1) == '#')
            {
                title_end--;
            }

            while (
                title_end > title_start &&
                apr_isspace(*(title_end - 1)))
            {
                title_end--;
            }

            if (
                title_end >
                title_start)
            {
                return apr_pstrndup(
                    r->pool,
                    title_start,
                    title_end -
                    title_start);
            }
        }

        if (*line_end == '\0')
        {
            break;
        }

        p =
            line_end + 1;
    }

    return NULL;
}


static const char *markdown_build_raw_url(
    request_rec *r,
    markdown_dir_config *config)
{
    const char *uri =
        r->uri != NULL
            ? r->uri
            : "";

    const char *parameter =
        config->raw_parameter != NULL
            ? config->raw_parameter
            : "raw";

    if (
        r->args != NULL &&
        *r->args != '\0')
    {
        return apr_pstrcat(
            r->pool,
            uri,
            "?",
            r->args,
            "&",
            parameter,
            "=1",
            NULL);
    }

    return apr_pstrcat(
        r->pool,
        uri,
        "?",
        parameter,
        "=1",
        NULL);
}


static void markdown_write_html_document(
    request_rec *r,
    markdown_dir_config *config,
    const char *markdown_source,
    const char *html)
{
    const char *title =
        "Markdown";

    if (
        config->title_from_h1)
    {
        const char *h1 =
            markdown_extract_h1(
                r,
                markdown_source);

        if (
            h1 != NULL &&
            *h1 != '\0')
        {
            title = h1;
        }
    }

    const char *escaped_title =
        markdown_escape_html(
            r,
            title);

    ap_rputs(
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" "
        "content=\"width=device-width, initial-scale=1\">\n",
        r);

    ap_rprintf(
        r,
        "<title>%s</title>\n",
        escaped_title);

    if (
        config->stylesheet != NULL &&
        *config->stylesheet != '\0')
    {
        ap_rprintf(
            r,
            "<link rel=\"stylesheet\" href=\"%s\">\n",
            markdown_escape_html(
                r,
                config->stylesheet));
    }
    else
    {
        ap_rputs(
            "<style>\n"
            "body {"
            " max-width: 900px;"
            " margin: 40px auto;"
            " padding: 0 24px;"
            " font-family: -apple-system, BlinkMacSystemFont,"
            " \"Segoe UI\", sans-serif;"
            " line-height: 1.6;"
            " color: #222;"
            "}\n"
            "pre {"
            " overflow-x: auto;"
            " padding: 16px;"
            " background: #f6f8fa;"
            " border-radius: 6px;"
            "}\n"
            "code {"
            " font-family: Consolas, Monaco, monospace;"
            "}\n"
            "table {"
            " border-collapse: collapse;"
            "}\n"
            "th, td {"
            " border: 1px solid #ddd;"
            " padding: 6px 12px;"
            "}\n"
            ".markdown-raw-link {"
            " margin-top: 48px;"
            " padding-top: 16px;"
            " border-top: 1px solid #ddd;"
            " font-size: 0.9em;"
            "}\n"
            "</style>\n",
            r);
    }

    ap_rputs(
        "</head>\n"
        "<body>\n",
        r);

    ap_rputs(
        html,
        r);

    if (
        config->raw_link_enabled)
    {
        const char *raw_url =
            markdown_build_raw_url(
                r,
                config);

        ap_rprintf(
            r,
            "\n<div class=\"markdown-raw-link\">"
            "<a href=\"%s\">View raw Markdown</a>"
            "</div>\n",
            markdown_escape_html(
                r,
                raw_url));
    }

    ap_rputs(
        "</body>\n"
        "</html>\n",
        r);
}


/*
 * ==========================================================================
 * Main handler
 * ==========================================================================
 */

static int markdown_handler(
    request_rec *r)
{
    if (
        r->handler == NULL ||
        strcmp(
            r->handler,
            MOD_MARKDOWN_HANDLER) != 0)
    {
        return DECLINED;
    }

    /*
     * Apache represents HEAD using M_GET plus r->header_only,
     * so this allows both GET and HEAD.
     */
    if (
        r->method_number != M_GET)
    {
        apr_table_setn(
            r->headers_out,
            "Allow",
            "GET, HEAD");

        return HTTP_METHOD_NOT_ALLOWED;
    }

    markdown_dir_config *config =
        ap_get_module_config(
            r->per_dir_config,
            &markdown_module);

    if (
        config == NULL ||
        !config->enabled)
    {
        return DECLINED;
    }

    /*
     * Determine representation before generating the ETag.
     */
    int html =
        markdown_wants_html(
            r,
            config);

    if (html < 0)
    {
        return HTTP_NOT_ACCEPTABLE;
    }

    /*
     * Stat the file first.
     *
     * This is sufficient to build Last-Modified and ETag,
     * allowing a 304 or other conditional response before
     * opening or parsing the Markdown file.
     */
    apr_finfo_t info;

    apr_status_t status =
        markdown_get_file_info(
            r,
            &info);

    if (
        status != APR_SUCCESS ||
        info.filetype != APR_REG)
    {
        return HTTP_NOT_FOUND;
    }

    /*
     * Set response representation type before condition processing.
     */
    if (html)
    {
        ap_set_content_type(
            r,
            "text/html; charset=utf-8");
    }
    else
    {
        ap_set_content_type(
            r,
            "text/markdown; charset=utf-8");
    }

    /*
     * Populate:
     *
     *   r->mtime
     *   Last-Modified
     *   ETag
     *   Vary: Accept
     */
    markdown_set_cache_validators(
        r,
        &info,
        html);

    /*
     * Let Apache evaluate HTTP request preconditions.
     *
     * This handles the normal conditional headers such as:
     *
     *   If-Match
     *   If-None-Match
     *   If-Modified-Since
     *   If-Unmodified-Since
     *
     * according to Apache's HTTP core behavior.
     *
     * Possible responses include:
     *
     *   OK
     *   HTTP_NOT_MODIFIED
     *   HTTP_PRECONDITION_FAILED
     *
     * If anything other than OK is returned, no body needs
     * to be read or rendered.
     */
    int condition_status =
        ap_meets_conditions(r);

    if (
        condition_status != OK)
    {
        return condition_status;
    }

    /*
     * HEAD requests require the same metadata/conditional handling,
     * but no entity body.
     */
    if (r->header_only)
    {
        return OK;
    }

    /*
     * Only now do we read the Markdown file.
     */
    char *markdown;
    apr_size_t markdown_length;

    status =
        markdown_read_file(
            r,
            &info,
            &markdown,
            &markdown_length);

    if (
        status != APR_SUCCESS)
    {
        ap_log_rerror(
            APLOG_MARK,
            APLOG_ERR,
            status,
            r,
            "mod_markdown: unable to read file %s",
            r->filename);

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /*
     * Raw Markdown.
     */
    if (!html)
    {
        ap_rwrite(
            markdown,
            (int)markdown_length,
            r);

        return OK;
    }

    /*
     * Render HTML.
     */
    char *html_output =
        cmark_markdown_to_html(
            markdown,
            markdown_length,
            CMARK_OPT_DEFAULT);

    if (
        html_output == NULL)
    {
        ap_log_rerror(
            APLOG_MARK,
            APLOG_ERR,
            0,
            r,
            "mod_markdown: cmark failed parsing %s",
            r->filename);

        return HTTP_INTERNAL_SERVER_ERROR;
    }

    markdown_write_html_document(
        r,
        config,
        markdown,
        html_output);

    free(html_output);

    return OK;
}


/*
 * ==========================================================================
 * Fixup hook
 * ==========================================================================
 */

static int markdown_fixups(
    request_rec *r)
{
    markdown_dir_config *config =
        ap_get_module_config(
            r->per_dir_config,
            &markdown_module);

    if (
        config == NULL ||
        !config->enabled)
    {
        return DECLINED;
    }

    if (
        r->handler != NULL &&
        strcmp(
            r->handler,
            "default-handler") != 0)
    {
        return DECLINED;
    }

    if (
        markdown_has_supported_extension(
            r,
            config))
    {
        r->handler =
            MOD_MARKDOWN_HANDLER;
    }

    return DECLINED;
}


/*
 * ==========================================================================
 * Directives
 * ==========================================================================
 */

static const command_rec markdown_commands[] =
{
    AP_INIT_FLAG(
        "MarkdownEngine",
        markdown_set_enabled,
        NULL,
        OR_FILEINFO,
        "Enable or disable mod_markdown"),

    AP_INIT_TAKE1(
        "MarkdownExtension",
        markdown_add_extension,
        NULL,
        OR_FILEINFO,
        "Add an extension handled as Markdown"),

    AP_INIT_FLAG(
        "MarkdownHtml",
        markdown_set_html_enabled,
        NULL,
        OR_FILEINFO,
        "Enable or disable HTML rendering"),

    AP_INIT_FLAG(
        "MarkdownRawLink",
        markdown_set_raw_link_enabled,
        NULL,
        OR_FILEINFO,
        "Include a link to the raw Markdown in HTML output"),

    AP_INIT_FLAG(
        "MarkdownTitleFromH1",
        markdown_set_title_from_h1,
        NULL,
        OR_FILEINFO,
        "Use the first Markdown H1 as the HTML title"),

    AP_INIT_TAKE1(
        "MarkdownStylesheet",
        markdown_set_stylesheet,
        NULL,
        OR_FILEINFO,
        "URL of stylesheet included in rendered HTML"),

    AP_INIT_TAKE1(
        "MarkdownRawParameter",
        markdown_set_raw_parameter,
        NULL,
        OR_FILEINFO,
        "Query-string parameter used to request raw Markdown"),

    { NULL }
};


/*
 * ==========================================================================
 * Hook registration
 * ==========================================================================
 */

static void markdown_register_hooks(
    apr_pool_t *pool)
{
    ap_hook_fixups(
        markdown_fixups,
        NULL,
        NULL,
        APR_HOOK_MIDDLE);

    ap_hook_handler(
        markdown_handler,
        NULL,
        NULL,
        APR_HOOK_MIDDLE);
}


/*
 * ==========================================================================
 * Module declaration
 * ==========================================================================
 */

module AP_MODULE_DECLARE_DATA markdown_module =
{
    STANDARD20_MODULE_STUFF,

    markdown_create_dir_config,
    NULL,

    NULL,
    NULL,

    markdown_commands,
    markdown_register_hooks
};
