extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>
}

#include <v8.h>
#include <dlfcn.h>
#include <iostream>

using namespace std;
using namespace v8;

extern ngx_module_t  ngx_http_v8_module;

static char *ngx_http_v8(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_v8com(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void *ngx_http_v8_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_v8_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static Handle<Value> Log(const Arguments& args);
static Handle<Value> Write(const Arguments& args);
static void *Unwrap(Handle<Object> obj, int field);

typedef struct {
    Persistent<Context> context;
    Persistent<Function> process;
    Persistent<ObjectTemplate> classes;
    Persistent<ObjectTemplate> interfaces;
    Persistent<ObjectTemplate> request_tmpl;
    Persistent<ObjectTemplate> response_tmpl;
} ngx_http_v8_loc_conf_t;

typedef struct {
    ngx_chain_t *head;
    ngx_chain_t *last;
} brigade_t;

static ngx_command_t  ngx_http_v8_commands[] = {

    { ngx_string("v8"),
        NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF|NGX_CONF_TAKE1,
        ngx_http_v8,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL },

    { ngx_string("v8com"),
        NGX_HTTP_LOC_CONF|NGX_HTTP_LMT_CONF|NGX_CONF_TAKE1,
        ngx_http_v8com,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL },

    ngx_null_command
};

static ngx_http_module_t ngx_http_v8_module_ctx = {
    NULL,                                /* preconfiguration */
    NULL,                                /* postconfiguration */

    NULL,                                /* create main configuration */
    NULL,                                /* init main configuration */

    NULL,                                /* create server configuration */
    NULL,                                /* merge server configuration */

    ngx_http_v8_create_loc_conf,         /* create location configuration */
    ngx_http_v8_merge_loc_conf           /* merge location configuration */

};

ngx_module_t  ngx_http_v8_module = {
    NGX_MODULE_V1,
    &ngx_http_v8_module_ctx,       /* module context */
    ngx_http_v8_commands,          /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};

static Handle<String> ReadFile(const string& name) {
    FILE* file = fopen(name.c_str(), "rb");
    if (file == NULL) return Handle<String>();

    fseek(file, 0, SEEK_END);
    int size = ftell(file);
    rewind(file);

    char* chars = new char[size + 1];
    chars[size] = '\0';
    for (int i = 0; i < size;) {
        int read = fread(&chars[i], 1, size - i, file);
        i += read;
    }
    fclose(file);
    Handle<String> result = String::New(chars, size);
    delete[] chars;
    return result;
}

static Handle<Value> GetUri(Local<String> name,
                      const AccessorInfo& info)
{
    ngx_http_request_t *r = static_cast<ngx_http_request_t*>(Unwrap(info.Holder(), 0));
    ngx_str_t uri = r->uri;
    return String::New(reinterpret_cast<const char*>(uri.data), uri.len);
}

static Handle<Value> GetUserAgent(Local<String> name,
                      const AccessorInfo& info)
{
    ngx_http_request_t *r = static_cast<ngx_http_request_t*>(Unwrap(info.Holder(), 0));
    ngx_str_t agent = r->headers_in.user_agent->value;
    return String::New(reinterpret_cast<const char*>(agent.data), agent.len);

}
static Handle<Value> GetArgs(Local<String> name,
                      const AccessorInfo& info)
{
    ngx_http_request_t *r = static_cast<ngx_http_request_t*>(Unwrap(info.Holder(), 0));
    ngx_str_t args = r->args;
    return String::New(reinterpret_cast<const char*>(args.data), args.len);
}

static Handle<Value> GetRespContentType(Local<String> name,
                      const AccessorInfo& info)
{
    ngx_http_request_t *r = static_cast<ngx_http_request_t*>(Unwrap(info.Holder(), 0));
    return String::New(reinterpret_cast<const char*>(r->headers_out.content_type.data),
        r->headers_out.content_type.len);
}

void SetRespContentType(Local<String> name,
                      Local<Value> val,
                      const AccessorInfo& info)
{
    ngx_http_request_t *r = static_cast<ngx_http_request_t*>(Unwrap(info.Holder(), 0));
    String::Utf8Value value(val);
    size_t len = strlen(*value);
    u_char *p = static_cast<u_char*>(ngx_pnalloc(r->pool, len));
    ngx_memcpy(p, *value, len);
    r->headers_out.content_type.data = p;
    r->headers_out.content_type.len = len;
}

static Handle<ObjectTemplate> MakeResponseTemplate()
{
    HandleScope handle_scope;
    Handle<ObjectTemplate> result = ObjectTemplate::New();
    result->SetInternalFieldCount(2);
    result->Set(String::New("write"), FunctionTemplate::New(Write));
    result->SetAccessor(String::NewSymbol("contentType"), GetRespContentType, SetRespContentType);
    return handle_scope.Close(result);
}

static Handle<ObjectTemplate> MakeRequestTemplate()
{
    HandleScope handle_scope;
    Handle<ObjectTemplate> result = ObjectTemplate::New();
    result->SetInternalFieldCount(1);
    result->SetAccessor(String::NewSymbol("uri"), GetUri);
    result->SetAccessor(String::NewSymbol("userAgent"), GetUserAgent);
    result->SetAccessor(String::NewSymbol("args"), GetArgs);
    return handle_scope.Close(result);
}

static Handle<Object> WrapRequest(ngx_http_v8_loc_conf_t *v8lcf,
                           ngx_http_request_t *r)
{
    HandleScope handle_scope;
    if (v8lcf->request_tmpl.IsEmpty()) {
        Handle<ObjectTemplate> raw_template = MakeRequestTemplate();
        v8lcf->request_tmpl = Persistent<ObjectTemplate>::New(raw_template);
    }
    Handle<ObjectTemplate> tmpl = v8lcf->request_tmpl;
    Handle<Object> result = tmpl->NewInstance();
    Handle<External> request_ptr = External::New(r);
    result->SetInternalField(0, request_ptr);
    return handle_scope.Close(result);
}

static Handle<Object> WrapResponse(ngx_http_v8_loc_conf_t *v8lcf,
                           ngx_http_request_t *r,
                           brigade_t *b)
{
    HandleScope handle_scope;
    if (v8lcf->response_tmpl.IsEmpty()) {
        Handle<ObjectTemplate> raw_template = MakeResponseTemplate();
        v8lcf->response_tmpl = Persistent<ObjectTemplate>::New(raw_template);
    }
    Handle<ObjectTemplate> tmpl = v8lcf->response_tmpl;
    Handle<Object> result = tmpl->NewInstance();
    Handle<External> request_ptr = External::New(r);
    Handle<External> chain_ptr = External::New(b);
    result->SetInternalField(0, request_ptr);
    result->SetInternalField(1, chain_ptr);
    return handle_scope.Close(result);
}

static void *Unwrap(Handle<Object> obj, int field)
{
    return Handle<External>::Cast(obj->GetInternalField(field))->Value();
}

static ngx_int_t
ngx_http_v8_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    brigade_t b;
    ngx_http_v8_loc_conf_t *v8lcf;

    v8lcf = static_cast<ngx_http_v8_loc_conf_t *>(
        ngx_http_get_module_loc_conf(r, ngx_http_v8_module));

    b.head = b.last = NULL;
    //new Locker();
    HandleScope handle_scope;
    Context::Scope context_scope(v8lcf->context);
    Handle<Object> request_obj = WrapRequest(v8lcf, r);
    Handle<Object> response_obj = WrapResponse(v8lcf, r, &b);
    Handle<Value> argv[2] = { request_obj, response_obj };
    Handle<Value> result = v8lcf->process->Call(v8lcf->context->Global(), 2, argv);

    r->headers_out.status = result->IsUndefined() ? NGX_HTTP_OK : result->Int32Value();
    if (r->headers_out.content_type.data == NULL) {
        r->headers_out.content_type.len = sizeof("text/html; charset=utf-8") - 1;
        r->headers_out.content_type.data = reinterpret_cast<u_char *>(
            const_cast<char *>("text/html; charset=utf-8"));
    }
    rc = ngx_http_send_header(r);

    if (b.head == NULL) {
        ngx_http_send_special(r, NGX_HTTP_LAST);
        ngx_http_finalize_request(r, NGX_HTTP_OK);
        return NGX_DONE;
    }
    return ngx_http_output_filter(r, b.head);
}

static Handle<Value> Write(const Arguments& args)
{
    HandleScope scope;
    Handle<Value> arg = args[0];
    String::Utf8Value value(arg);
    Handle<Object> self = args.This();
    ngx_http_request_t *r = static_cast<ngx_http_request_t*>(Unwrap(self, 0));
    brigade_t *bri = static_cast<brigade_t*>(Unwrap(self, 1));
    ngx_chain_t *out;

    ngx_buf_t *b;
    size_t len = strlen(*value);
    u_char *p = static_cast<u_char*>(ngx_pnalloc(r->pool, len));
    ngx_memcpy(p, *value, len);

    b = static_cast<ngx_buf_t *>(ngx_pcalloc(r->pool, sizeof(ngx_buf_t)));
    if (b == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
            "Failed to allocate response buffer.");
    }
    b->memory = 1;
    b->pos = p;
    b->last = p + len;
    b->last_buf = 1;

    out = ngx_alloc_chain_link(r->pool);
    out->buf = b;
    out->next = NULL;

    if (bri->head == NULL) {
        bri->head = bri->last = out;
    } else {
        bri->last->buf->last_buf = 0;
        bri->last->next = out;
        bri->last = out;
    }

    return Undefined();
}

static Handle<Value> Log(const Arguments& args)
{
    HandleScope scope;
    Handle<Value> arg = args[0];
    String::Utf8Value value(arg);
    cout << (*value) << endl;
    return Undefined();
}

static Handle<Value> Lookup(const Arguments& args) 
{
    return Undefined();
}

/*static Handle<ObjectTemplate> createV8Com()
{
    HandleScope scope;
    Handle<ObjectTemplate> v8com = ObjectTemplate::New();
    Handle<ObjectTemplate> components = ObjectTemplate::New();
    Handle<ObjectTemplate> classes = ObjectTemplate::New();
    Handle<ObjectTemplate> interfaces = ObjectTemplate::New();
    return scope.Close(v8com);
}*/

static void *
ngx_http_v8_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_v8_loc_conf_t   *conf;

    conf = static_cast<ngx_http_v8_loc_conf_t*>(
        ngx_pcalloc(cf->pool, sizeof(ngx_http_v8_loc_conf_t)));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    return conf;
}

static char*
ngx_http_v8_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    return NGX_CONF_OK;
}

static char *
ngx_http_v8(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_v8_loc_conf_t     *v8lcf;
    ngx_http_core_loc_conf_t   *clcf;
    ngx_str_t                  *value;

    v8lcf = static_cast<ngx_http_v8_loc_conf_t *>(conf);
    value = static_cast<ngx_str_t *>(cf->args->elts);

    clcf = static_cast<ngx_http_core_loc_conf_t *>(
        ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module));
    clcf->handler = ngx_http_v8_handler;

    HandleScope handle_scope;
    Handle<ObjectTemplate> global = ObjectTemplate::New();
    Handle<ObjectTemplate> components = ObjectTemplate::New();
    global->Set(String::New("log"), FunctionTemplate::New(Log));
    components->Set(String::New("classes"), v8lcf->classes);
    components->Set(String::New("interfaces"), v8lcf->interfaces);
    components->Set(String::New("lookup"), FunctionTemplate::New(Lookup));
    global->Set(String::New("Components"), components);
    
    Handle<Context> context = Context::New(NULL, global);
    v8lcf->context = Persistent<Context>::New(context);

    Context::Scope context_scope(context);
    /*Handle<String> mochibase = ReadFile("/home/rykomats/tmp/MochiKit-1.4/lib/MochiKit/Base.js");
    Handle<Script> mochiscript = Script::Compile(mochibase);
    Handle<Value> mochiresult = mochiscript->Run();
    Handle<String> mochiiter = ReadFile("/home/rykomats/tmp/MochiKit-1.4/lib/MochiKit/Iter.js");
    mochiscript = Script::Compile(mochiiter);
    mochiresult = mochiscript->Run();*/
    Handle<String> source = ReadFile(reinterpret_cast<const char *>(value[1].data));
    Handle<Script> script = Script::Compile(source);
    Handle<Value> result = script->Run();
    Handle<String> process_name = String::New("process");
    Handle<Value> process_val = context->Global()->Get(process_name);
    Handle<Function> process_fun = Handle<Function>::Cast(process_val);
    v8lcf->process = Persistent<Function>::New(process_fun);

    return NGX_CONF_OK;
}

static char *
ngx_http_v8com(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_v8_loc_conf_t     *v8lcf;
    ngx_str_t                  *value;
    void                       *handle;

    v8lcf = static_cast<ngx_http_v8_loc_conf_t *>(conf);
    value = static_cast<ngx_str_t *>(cf->args->elts);

    HandleScope handle_scope;
    if (v8lcf->classes.IsEmpty()) {
        Handle<ObjectTemplate> classes = ObjectTemplate::New();
        v8lcf->classes = Persistent<ObjectTemplate>::New(classes);
    }
    if (v8lcf->interfaces.IsEmpty()) {
        Handle<ObjectTemplate> interfaces = ObjectTemplate::New();
        v8lcf->interfaces = Persistent<ObjectTemplate>::New(interfaces);
    }

    handle = dlopen(reinterpret_cast<const char *>(value[1].data), RTLD_LAZY);
    Handle<String>(*getName)();
    Handle<Template>(*createObject)();
    getName = reinterpret_cast<Handle<String> (*)()>(dlsym(handle, "getName"));
    createObject = reinterpret_cast<Handle<Template> (*)()>(dlsym(handle, "createObject"));
    v8lcf->classes->Set(getName(), createObject());
    //dlclose(handle);

    return NGX_CONF_OK;
}