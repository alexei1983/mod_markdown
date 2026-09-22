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
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MOD_MARKDOWN_VERSION "1.1.0"

typedef struct {
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

static const char *html_escape(apr_pool_t *p,const char *s){const char *q;apr_size_t n=0;char *o,*d;if(!s)return "";for(q=s;*q;q++)n+=(*q=='&'?5:(*q=='<'||*q=='>'?4:(*q=='\"'?6:1)));d=o=apr_palloc(p,n+1);for(q=s;*q;q++){if(*q=='&'){memcpy(d,"&amp;",5);d+=5;}else if(*q=='<'){memcpy(d,"&lt;",4);d+=4;}else if(*q=='>'){memcpy(d,"&gt;",4);d+=4;}else if(*q=='\"'){memcpy(d,"&quot;",6);d+=6;}else *d++=*q;}*d='\0';return o;}

static apr_status_t render_impl(request_rec *r,const char *markdown,apr_size_t markdown_len,const markdown_render_options *opts,const char **out,apr_size_t *out_len){cmark_parser *parser; cmark_node *doc; char *fragment; cmark_llist *extensions; int flags=CMARK_OPT_DEFAULT; const char *result;if(!markdown||!out||!out_len)return APR_EINVAL;cmark_gfm_core_extensions_ensure_registered();parser=cmark_parser_new(flags);if(!parser)return APR_ENOMEM;{const char *names[]={"table","strikethrough","autolink","tagfilter","tasklist",NULL};int i;for(i=0;names[i];i++){cmark_syntax_extension *e=cmark_find_syntax_extension(names[i]);if(e)cmark_parser_attach_syntax_extension(parser,e);}}cmark_parser_feed(parser,markdown,markdown_len);doc=cmark_parser_finish(parser);if(!doc){cmark_parser_free(parser);return APR_EGENERAL;}extensions=cmark_parser_get_syntax_extensions(parser);fragment=cmark_render_html(doc,flags,extensions);if(!fragment){cmark_node_free(doc);cmark_parser_free(parser);return APR_EGENERAL;}if(opts&&(opts->flags&MARKDOWN_RENDER_FULL_DOCUMENT)){const char *title=html_escape(r->pool,opts->title?opts->title:"Markdown");const char *style=opts->stylesheet?apr_psprintf(r->pool,"<link rel=\"stylesheet\" href=\"%s\">\n",html_escape(r->pool,opts->stylesheet)):"";const char *raw=opts->raw_uri?apr_psprintf(r->pool,"<link rel=\"alternate\" type=\"text/markdown\" href=\"%s\">\n",html_escape(r->pool,opts->raw_uri)):"";result=apr_pstrcat(r->pool,"<!doctype html>\n<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n<title>",title,"</title>\n",style,raw,"</head><body>\n",fragment,"\n</body></html>\n",NULL);}else result=apr_pstrdup(r->pool,fragment);free(fragment);cmark_node_free(doc);cmark_parser_free(parser);*out=result;*out_len=strlen(result);return APR_SUCCESS;}

static markdown_renderer_provider_v1 renderer={MARKDOWN_PROVIDER_ABI,sizeof(markdown_renderer_provider_v1),render_impl};

static void *create_cfg(apr_pool_t *p,char *d){markdown_cfg*c=apr_pcalloc(p,sizeof(*c));c->enabled=1;c->full_document=1;c->raw_link=1;c->title_from_h1=1;c->max_size=2*1024*1024;c->raw_parameter="raw";return c;}
static const char *set_flag(cmd_parms *cmd,void *v,int on){markdown_cfg*c=v;if(!strcasecmp(cmd->cmd->name,"MarkdownHtml"))c->enabled=on;else if(!strcasecmp(cmd->cmd->name,"MarkdownFullDocument"))c->full_document=on;else if(!strcasecmp(cmd->cmd->name,"MarkdownRawLink"))c->raw_link=on;else c->title_from_h1=on;return NULL;}
static const char *set_string(cmd_parms *cmd,void *v,const char *s){markdown_cfg*c=v;if(!strcasecmp(cmd->cmd->name,"MarkdownStylesheet"))c->stylesheet=s;else if(!strcasecmp(cmd->cmd->name,"MarkdownRawParameter"))c->raw_parameter=s;else c->preprocessor=!strcasecmp(s,"none")?NULL:s;return NULL;}
static const char *set_size(cmd_parms *cmd,void *v,const char *s){markdown_cfg*c=v;char *end;long n=strtol(s,&end,10);if(*end||n<1)return "MarkdownMaxSize must be a positive integer";c->max_size=(apr_off_t)n;return NULL;}
static const command_rec commands[]={AP_INIT_FLAG("MarkdownHtml",set_flag,NULL,OR_FILEINFO,"Enable Markdown HTML rendering"),AP_INIT_FLAG("MarkdownFullDocument",set_flag,NULL,OR_FILEINFO,"Render a complete HTML document"),AP_INIT_FLAG("MarkdownRawLink",set_flag,NULL,OR_FILEINFO,"Include a raw Markdown link"),AP_INIT_FLAG("MarkdownTitleFromH1",set_flag,NULL,OR_FILEINFO,"Use the first H1 as the page title"),AP_INIT_TAKE1("MarkdownStylesheet",set_string,NULL,OR_FILEINFO,"Stylesheet URL"),AP_INIT_TAKE1("MarkdownRawParameter",set_string,NULL,OR_FILEINFO,"Query parameter that requests raw Markdown"),AP_INIT_TAKE1("MarkdownPreprocessor",set_string,NULL,OR_FILEINFO,"Preprocessor provider name or none"),AP_INIT_TAKE1("MarkdownMaxSize",set_size,NULL,OR_FILEINFO,"Maximum Markdown file size"),{NULL}};

static int accepts_html(request_rec *r){const char *a=apr_table_get(r->headers_in,"Accept");return a&&strstr(a,"text/html");}
static const char *first_h1(apr_pool_t *p,const char *s,apr_size_t len){const char *end=s+len,*q,*e;for(q=s;q<end;){e=memchr(q,'\n',(apr_size_t)(end-q));if(!e)e=end;if(e-q>2&&q[0]=='#'&&q[1]==' ')return apr_pstrndup(p,q+2,(apr_size_t)(e-q-2));q=e<end?e+1:end;}return NULL;}
static int markdown_handler(request_rec *r){markdown_cfg*c=ap_get_module_config(r->per_dir_config,&markdown_module);apr_file_t*f;apr_finfo_t fi;apr_table_t*args=NULL;char *buf;apr_size_t n,sent=0;const char *input,*html,*title,*raw_uri;apr_size_t input_len,html_len;markdown_render_options opts;const markdown_preprocessor_provider_v1 *pre=NULL;int cond,raw=0;if(!c||!c->enabled||strcmp(r->handler,"markdown"))return DECLINED;if(strcmp(r->method,"GET")&&strcmp(r->method,"HEAD"))return HTTP_METHOD_NOT_ALLOWED;if(!r->filename||apr_stat(&fi,r->filename,APR_FINFO_SIZE|APR_FINFO_MTIME,r->pool)!=APR_SUCCESS)return HTTP_NOT_FOUND;if(fi.size<0||fi.size>c->max_size)return HTTP_REQUEST_ENTITY_TOO_LARGE;if(apr_file_open(&f,r->filename,APR_READ|APR_BINARY,APR_OS_DEFAULT,r->pool)!=APR_SUCCESS)return HTTP_NOT_FOUND;r->finfo=fi;r->mtime=fi.mtime;ap_set_last_modified(r);ap_set_etag(r);apr_table_merge(r->headers_out,"Vary","Accept");cond=ap_meets_conditions(r);if(cond!=OK){apr_file_close(f);return cond;}if(c->raw_parameter&&r->args){ap_args_to_table(r,&args);raw=apr_table_get(args,c->raw_parameter)!=NULL;}if(raw||!accepts_html(r)){ap_set_content_type(r,"text/markdown; charset=utf-8");apr_table_setn(r->headers_out,"Link",apr_psprintf(r->pool,"<%s>; rel=\"alternate\"; type=\"text/html\"",r->uri));if(strcmp(r->method,"HEAD"))ap_send_fd(f,r,0,(apr_size_t)fi.size,&sent);apr_file_close(f);return OK;}
    n=(apr_size_t)fi.size;buf=apr_palloc(r->pool,n+1);if(apr_file_read_full(f,buf,n,NULL)!=APR_SUCCESS){apr_file_close(f);return HTTP_INTERNAL_SERVER_ERROR;}apr_file_close(f);buf[n]='\0';input=buf;input_len=n;if(c->preprocessor){pre=ap_lookup_provider(MARKDOWN_PREPROCESSOR_PROVIDER_GROUP,c->preprocessor,MARKDOWN_PROVIDER_VERSION);if(!pre||pre->abi_version!=MARKDOWN_PROVIDER_ABI||pre->struct_size<sizeof(*pre)||!pre->process)return HTTP_INTERNAL_SERVER_ERROR;if(pre->process(r,input,input_len,&input,&input_len)!=APR_SUCCESS)return HTTP_BAD_REQUEST;}title=c->title_from_h1?first_h1(r->pool,input,input_len):NULL;if(!title)title=r->filename;raw_uri=c->raw_link?apr_psprintf(r->pool,"%s?%s=1",r->uri,c->raw_parameter):NULL;memset(&opts,0,sizeof(opts));if(c->full_document)opts.flags|=MARKDOWN_RENDER_FULL_DOCUMENT;opts.title=title;opts.stylesheet=c->stylesheet;opts.raw_uri=raw_uri;if(render_impl(r,input,input_len,&opts,&html,&html_len)!=APR_SUCCESS)return HTTP_INTERNAL_SERVER_ERROR;ap_set_content_type(r,"text/html; charset=utf-8");apr_table_setn(r->headers_out,"Link",apr_psprintf(r->pool,"<%s>; rel=\"alternate\"; type=\"text/markdown\"",raw_uri?raw_uri:r->uri));if(strcmp(r->method,"HEAD"))ap_rwrite(html,html_len,r);return OK;}

static void register_hooks(apr_pool_t *p){ap_register_provider(p,MARKDOWN_RENDERER_PROVIDER_GROUP,"cmark-gfm",MARKDOWN_PROVIDER_VERSION,&renderer);ap_hook_handler(markdown_handler,NULL,NULL,APR_HOOK_MIDDLE);}
module AP_MODULE_DECLARE_DATA markdown_module={STANDARD20_MODULE_STUFF,create_cfg,NULL,NULL,NULL,commands,register_hooks};
