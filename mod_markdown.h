#ifndef MOD_MARKDOWN_H
#define MOD_MARKDOWN_H

#include "httpd.h"
#include "apr_pools.h"

#define MARKDOWN_RENDERER_PROVIDER_GROUP "markdown-renderer"
#define MARKDOWN_PREPROCESSOR_PROVIDER_GROUP "markdown-preprocessor"
#define MARKDOWN_PROVIDER_VERSION "1"
#define MARKDOWN_PROVIDER_ABI 0x00010000u

#define MARKDOWN_RENDER_FULL_DOCUMENT 0x0001u

typedef struct markdown_render_options {
    unsigned int flags;
    const char *title;
    const char *stylesheet;
    const char *raw_uri;
} markdown_render_options;

typedef struct markdown_renderer_provider_v1 {
    unsigned int abi_version;
    apr_size_t struct_size;
    apr_status_t (*render)(request_rec *, const char *, apr_size_t,
                           const markdown_render_options *,
                           const char **, apr_size_t *);
} markdown_renderer_provider_v1;

typedef struct markdown_preprocessor_provider_v1 {
    unsigned int abi_version;
    apr_size_t struct_size;
    apr_status_t (*process)(request_rec *, const char *, apr_size_t,
                            const char **, apr_size_t *);
} markdown_preprocessor_provider_v1;

#endif
