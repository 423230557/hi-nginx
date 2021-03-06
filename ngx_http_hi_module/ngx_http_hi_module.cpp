extern "C" {
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_md5.h>
}

#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/md5.h>
#include <openssl/x509v3.h>

#ifdef HTTP_HI_CPP

#include <vector>
#include <memory>
#include "include/request.hpp"
#include "include/response.hpp"
#include "include/servlet.hpp"



#include "lib/module_class.hpp"
#include "lib/lrucache.hpp"
#include "lib/param.hpp"
#include "lib/redis.hpp"
#include "lib/MPFDParser/Parser.h"

#ifdef HTTP_HI_PYTHON
#include "lib/py_request.hpp"
#include "lib/py_response.hpp"
#include "lib/pybind11_py.hpp"
#endif

#ifdef HTTP_HI_LUA
#include "lib/lua.hpp"
#endif

#ifdef HTTP_HI_JAVA
#include "lib/java.hpp"
#endif

#ifdef HTTP_HI_PHP
#include "lib/php-x/phpx.h"
#include "lib/php-x/phpx_embed.h"
#endif



#define SESSION_ID_NAME "SESSIONID"
#define form_urlencoded_type "application/x-www-form-urlencoded"
#define form_urlencoded_type_len (sizeof(form_urlencoded_type) - 1)
#define TEMP_DIRECTORY "temp"

struct cache_ele_t {
    int status = 200;
    time_t t;
    std::string content_type, content;
};

static std::vector<std::shared_ptr<hi::module_class<hi::servlet>>> PLUGIN;
static std::vector<std::shared_ptr<hi::cache::lru_cache<std::string, cache_ele_t>>> CACHE;
static std::shared_ptr<hi::redis> REDIS;

#ifdef HTTP_HI_PYTHON
static std::shared_ptr<hi::pybind11_py> PYTHON;
#endif

#ifdef HTTP_HI_LUA
static std::shared_ptr<hi::lua> LUA;
#endif

#ifdef HTTP_HI_JAVA
static std::shared_ptr<hi::java> JAVA;
static std::shared_ptr<hi::cache::lru_cache<std::string, hi::java_servlet_t>> JAVA_SERVLET_CACHE;
static bool JAVA_IS_READY = false;
#endif

#ifdef HTTP_HI_PHP
static std::shared_ptr<php::VM> PHP;
#endif

enum application_t {
    __cpp__,
#ifdef HTTP_HI_PYTHON
    __python__,
#endif
#ifdef HTTP_HI_LUA
    __lua__,
#endif
#ifdef HTTP_HI_JAVA    
    __java__,
#endif
#ifdef HTTP_HI_PHP
    __php__,
#endif
    __unkown__
};

typedef struct {
    ngx_str_t module_path
    , redis_host
#ifdef HTTP_HI_PYTHON
    , python_script
    , python_content
#endif
#ifdef HTTP_HI_LUA
    , lua_script
    , lua_content
#endif
#ifdef HTTP_HI_JAVA
    , java_classpath
    , java_options
    , java_servlet
#endif
#ifdef HTTP_HI_PHP
    , php_script
#endif
    ;
    ngx_int_t redis_port
    , module_index
    , cache_expires
    , session_expires
    , cache_index
#ifdef HTTP_HI_JAVA
    , java_servlet_cache_expires
    , java_version
#endif
    ;
    size_t cache_size
#ifdef HTTP_HI_JAVA
    , java_servlet_cache_size
#endif
    ;
    ngx_flag_t need_headers
    , need_cache
    , need_cookies
    , need_session;
    application_t app_type;
} ngx_http_hi_loc_conf_t;


static ngx_int_t clean_up(ngx_conf_t *cf);
static char *ngx_http_hi_conf_init(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void * ngx_http_hi_create_loc_conf(ngx_conf_t *cf);
static char * ngx_http_hi_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child);


static ngx_int_t ngx_http_hi_handler(ngx_http_request_t *r);
static void ngx_http_hi_body_handler(ngx_http_request_t* r);
static ngx_int_t ngx_http_hi_normal_handler(ngx_http_request_t *r);


static void get_input_headers(ngx_http_request_t* r, std::unordered_map<std::string, std::string>& input_headers);
static void set_output_headers(ngx_http_request_t* r, std::unordered_multimap<std::string, std::string>& output_headers);
static ngx_str_t get_input_body(ngx_http_request_t *r);

static void ngx_http_hi_cpp_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);

#ifdef HTTP_HI_PYTHON
static void ngx_http_hi_python_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);
#endif

#ifdef HTTP_HI_LUA
static void ngx_http_hi_lua_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);
#endif

#ifdef HTTP_HI_JAVA
static void ngx_http_hi_java_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);
static void java_input_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res, jobject request_instance, jobject response_instance);
static void java_output_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res, jobject request_instance, jobject response_instance);
static bool java_init_handler(ngx_http_hi_loc_conf_t * conf);
#endif

#ifdef HTTP_HI_PHP
static void ngx_http_hi_php_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res);
#endif

static std::string md5(const std::string& str);
static std::string random_string(const std::string& s);
static bool is_dir(const std::string& s);

ngx_command_t ngx_http_hi_commands[] = {
    {
        ngx_string("hi"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, module_path),
        NULL
    },
    {
        ngx_string("hi_cache_size"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_num_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, cache_size),
        NULL
    },
    {
        ngx_string("hi_cache_expires"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_sec_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, cache_expires),
        NULL
    },
    {
        ngx_string("hi_need_headers"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_headers),
        NULL
    },
    {
        ngx_string("hi_need_cache"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_cache),
        NULL
    },
    {
        ngx_string("hi_need_cookies"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_cookies),
        NULL
    },
    {
        ngx_string("hi_redis_host"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, redis_host),
        NULL
    },
    {
        ngx_string("hi_redis_port"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_num_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, redis_port),
        NULL
    },
    {
        ngx_string("hi_need_session"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, need_session),
        NULL
    },
    {
        ngx_string("hi_session_expires"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_sec_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, session_expires),
        NULL
    },
#ifdef HTTP_HI_PYTHON
    {
        ngx_string("hi_python_script"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, python_script),
        NULL
    },
    {
        ngx_string("hi_python_content"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, python_content),
        NULL
    },
#endif
#ifdef HTTP_HI_LUA
    {
        ngx_string("hi_lua_script"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, lua_script),
        NULL
    },
    {
        ngx_string("hi_lua_content"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, lua_content),
        NULL
    },
#endif
#ifdef HTTP_HI_JAVA
    {
        ngx_string("hi_java_classpath"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, java_classpath),
        NULL
    },
    {
        ngx_string("hi_java_options"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, java_options),
        NULL
    },
    {
        ngx_string("hi_java_servlet"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, java_servlet),
        NULL
    },
    {
        ngx_string("hi_java_servlet_cache_expires"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_sec_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, java_servlet_cache_expires),
        NULL
    },
    {
        ngx_string("hi_java_servlet_cache_size"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_num_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, java_servlet_cache_size),
        NULL
    },
    {
        ngx_string("hi_java_version"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_CONF_TAKE1,
        ngx_conf_set_num_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, java_version),
        NULL
    },
#endif
#ifdef HTTP_HI_PHP
    {
        ngx_string("hi_php_script"),
        NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_TAKE1,
        ngx_http_hi_conf_init,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_hi_loc_conf_t, php_script),
        NULL
    },
#endif
    ngx_null_command
};


ngx_http_module_t ngx_http_hi_module_ctx = {
    clean_up, /* preconfiguration */
    NULL, /* postconfiguration */
    NULL, /* create main configuration */
    NULL, /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    ngx_http_hi_create_loc_conf, /* create location configuration */
    ngx_http_hi_merge_loc_conf /* merge location configuration */
};




ngx_module_t ngx_http_hi_module = {
    NGX_MODULE_V1,
    &ngx_http_hi_module_ctx, /* module context */
    ngx_http_hi_commands, /* module directives */
    NGX_HTTP_MODULE, /* module type */
    NULL, /* init master */
    NULL, /* init module */
    NULL, /* init process */
    NULL, /* init thread */
    NULL, /* exit thread */
    NULL, /* exit process */
    NULL, /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t clean_up(ngx_conf_t *cf) {
    PLUGIN.clear();
    CACHE.clear();
    REDIS.reset();
#ifdef HTTP_HI_PYTHON
    PYTHON.reset();
#endif
#ifdef HTTP_HI_LUA
    LUA.reset();
#endif
#ifdef HTTP_HI_JAVA
    JAVA.reset();
    JAVA_SERVLET_CACHE.reset();
    JAVA_IS_READY = false;
#endif
#ifdef HTTP_HI_PHP
    PHP.reset();
#endif
    return NGX_OK;
}

static char *ngx_http_hi_conf_init(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) {
    ngx_http_core_loc_conf_t *clcf;
    clcf = (ngx_http_core_loc_conf_t *) ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_hi_handler;
    ngx_conf_set_str_slot(cf, cmd, conf);
    return NGX_CONF_OK;
}

static void * ngx_http_hi_create_loc_conf(ngx_conf_t *cf) {
    ngx_http_hi_loc_conf_t *conf = (ngx_http_hi_loc_conf_t*) ngx_pcalloc(cf->pool, sizeof (ngx_http_hi_loc_conf_t));
    if (conf) {
        conf->module_path.len = 0;
        conf->module_path.data = NULL;
        conf->module_index = NGX_CONF_UNSET;
        conf->redis_host.len = 0;
        conf->redis_host.data = NULL;
#ifdef HTTP_HI_PYTHON
        conf->python_script.len = 0;
        conf->python_script.data = NULL;
        conf->python_content.len = 0;
        conf->python_content.data = NULL;
#endif
#ifdef HTTP_HI_LUA
        conf->lua_script.len = 0;
        conf->lua_script.data = NULL;
        conf->lua_content.len = 0;
        conf->lua_content.data = NULL;
#endif
#ifdef HTTP_HI_PHP
        conf->php_script.len = 0;
        conf->php_script.data = NULL;
#endif
#ifdef HTTP_HI_JAVA
        conf->java_classpath.len = 0;
        conf->java_classpath.data = NULL;
        conf->java_servlet.len = 0;
        conf->java_servlet.data = NULL;
        conf->java_servlet_cache_size = NGX_CONF_UNSET_UINT;
        conf->java_servlet_cache_expires = NGX_CONF_UNSET;
        conf->java_version = NGX_CONF_UNSET;
#endif
        conf->redis_port = NGX_CONF_UNSET;
        conf->cache_size = NGX_CONF_UNSET_UINT;
        conf->cache_expires = NGX_CONF_UNSET;
        conf->session_expires = NGX_CONF_UNSET;
        conf->cache_index = NGX_CONF_UNSET;
        conf->need_headers = NGX_CONF_UNSET;
        conf->need_cache = NGX_CONF_UNSET;
        conf->need_cookies = NGX_CONF_UNSET;
        conf->need_session = NGX_CONF_UNSET;
        conf->app_type = application_t::__unkown__;
        return conf;
    }
    return NGX_CONF_ERROR;
}

static char * ngx_http_hi_merge_loc_conf(ngx_conf_t* cf, void* parent, void* child) {
    ngx_http_hi_loc_conf_t * prev = (ngx_http_hi_loc_conf_t*) parent;
    ngx_http_hi_loc_conf_t * conf = (ngx_http_hi_loc_conf_t*) child;

    ngx_conf_merge_str_value(conf->module_path, prev->module_path, "");
    ngx_conf_merge_str_value(conf->redis_host, prev->redis_host, "");
#ifdef HTTP_HI_PYTHON
    ngx_conf_merge_str_value(conf->python_script, prev->python_script, "");
    ngx_conf_merge_str_value(conf->python_content, prev->python_content, "");
#endif
#ifdef HTTP_HI_LUA
    ngx_conf_merge_str_value(conf->lua_script, prev->lua_script, "");
    ngx_conf_merge_str_value(conf->lua_content, prev->lua_content, "");
#endif
#ifdef HTTP_HI_PHP
    ngx_conf_merge_str_value(conf->php_script, prev->php_script, "");
#endif
#ifdef HTTP_HI_JAVA
    ngx_conf_merge_str_value(conf->java_classpath, prev->java_classpath, "-Djava.class.path=.");
    ngx_conf_merge_str_value(conf->java_options, prev->java_options, "-server -d64 -Xmx1G -Xms1G -Xmn256m");
    ngx_conf_merge_str_value(conf->java_servlet, prev->java_servlet, "");
    ngx_conf_merge_uint_value(conf->java_servlet_cache_size, prev->java_servlet_cache_size, (size_t) 10);
    ngx_conf_merge_sec_value(conf->java_servlet_cache_expires, prev->java_servlet_cache_expires, (ngx_int_t) 300);
#ifdef JNI_VERSION_9
    ngx_conf_merge_value(conf->java_version, prev->java_version, (ngx_int_t) 9);
#else
    ngx_conf_merge_value(conf->java_version, prev->java_version, (ngx_int_t) 8);
#endif
#endif
    ngx_conf_merge_value(conf->redis_port, prev->redis_port, (ngx_int_t) 0);
    ngx_conf_merge_uint_value(conf->cache_size, prev->cache_size, (size_t) 10);
    ngx_conf_merge_sec_value(conf->cache_expires, prev->cache_expires, (ngx_int_t) 300);
    ngx_conf_merge_sec_value(conf->session_expires, prev->session_expires, (ngx_int_t) 300);
    ngx_conf_merge_value(conf->need_headers, prev->need_headers, (ngx_flag_t) 0);
    ngx_conf_merge_value(conf->need_cache, prev->need_cache, (ngx_flag_t) 1);
    ngx_conf_merge_value(conf->need_cookies, prev->need_cookies, (ngx_flag_t) 0);
    ngx_conf_merge_value(conf->need_session, prev->need_session, (ngx_flag_t) 0);
    if (conf->need_session == 1 && conf->need_cookies == 0) {
        conf->need_cookies = 1;
    }
    if (conf->module_index == NGX_CONF_UNSET && conf->module_path.len > 0) {

        ngx_int_t index = NGX_CONF_UNSET;
        bool found = false;
        for (auto& item : PLUGIN) {
            ++index;
            if (item->get_module() == (char*) conf->module_path.data) {
                found = true;
                break;
            }
        }
        if (found) {
            conf->module_index = index;
        } else {
            PLUGIN.push_back(std::make_shared<hi::module_class < hi::servlet >> ((char*) conf->module_path.data));
            conf->module_index = PLUGIN.size() - 1;
        }
        conf->app_type = application_t::__cpp__;
    }
#ifdef HTTP_HI_PYTHON
    if (conf->python_content.len > 0 || conf->python_script.len > 0) {
        conf->app_type = application_t::__python__;
    }
#endif
#ifdef HTTP_HI_LUA
    if (conf->lua_content.len > 0 || conf->lua_script.len > 0) {
        conf->app_type = application_t::__lua__;
    }
#endif
#ifdef HTTP_HI_PHP
    if (conf->php_script.len > 0) {
        conf->app_type = application_t::__php__;
        if (!PHP) {
            int argc = 1;
            char* argv[2] = {"", NULL};
            PHP = std::move(std::make_shared<php::VM>(argc, argv));
        }
    }
#endif
#ifdef HTTP_HI_JAVA
    if (conf->java_servlet.len > 0) {
        conf->app_type = application_t::__java__;
        if (!JAVA_SERVLET_CACHE) {
            JAVA_SERVLET_CACHE = std::make_shared<hi::cache::lru_cache < std::string, hi::java_servlet_t >> (conf->java_servlet_cache_size);
        }
    }
#endif

    if (conf->need_cache == 1 && conf->cache_index == NGX_CONF_UNSET) {
        CACHE.push_back(std::make_shared<hi::cache::lru_cache < std::string, cache_ele_t >> (conf->cache_size));
        conf->cache_index = CACHE.size() - 1;
    }


    return NGX_CONF_OK;
}

static ngx_int_t ngx_http_hi_handler(ngx_http_request_t *r) {
    if (r->headers_in.content_length_n > 0) {
        ngx_http_core_loc_conf_t *clcf = (ngx_http_core_loc_conf_t *) ngx_http_get_module_loc_conf(r, ngx_http_core_module);
        if (clcf->client_body_buffer_size < (size_t) clcf->client_max_body_size) {
            clcf->client_body_buffer_size = clcf->client_max_body_size;
        }
        r->request_body_in_single_buf = 1;
        r->request_body_file_log_level = 0;
        ngx_int_t rc = ngx_http_read_client_request_body(r, ngx_http_hi_body_handler);
        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            return rc;
        }
        return NGX_DONE;
    } else {
        ngx_http_discard_request_body(r);
        return ngx_http_hi_normal_handler(r);
    }
}

static ngx_int_t ngx_http_hi_normal_handler(ngx_http_request_t *r) {

    ngx_http_hi_loc_conf_t * conf = (ngx_http_hi_loc_conf_t *) ngx_http_get_module_loc_conf(r, ngx_http_hi_module);

    if (r->headers_in.if_modified_since && r->headers_in.if_modified_since->value.data) {
        time_t now = time(NULL), old = ngx_http_parse_time(r->headers_in.if_modified_since->value.data, r->headers_in.if_modified_since->value.len);
        if (difftime(now, old) <= conf->cache_expires) {
            return NGX_HTTP_NOT_MODIFIED;
        }
    }

    hi::request ngx_request;
    hi::response ngx_response;
    std::string SESSION_ID_VALUE;

    ngx_request.uri.assign((char*) r->uri.data, r->uri.len);
    if (r->args.len > 0) {
        ngx_request.param.assign((char*) r->args.data, r->args.len);
    }
    std::shared_ptr<std::string> cache_k;
    if (conf->need_cache == 1) {
        ngx_response.headers.insert(std::make_pair("Last-Modified", (char*) ngx_cached_http_time.data));
        cache_k = std::make_shared<std::string>(ngx_request.uri);
        if (r->args.len > 0) {
            cache_k->append("?").append(ngx_request.param);
        }
        u_char *p;
        ngx_md5_t md5;
        u_char md5_buf[16];

        p = (u_char*) ngx_palloc(r->pool, 32);
        if (p == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_md5_init(&md5);
        ngx_md5_update(&md5, (u_char*) cache_k->c_str(), cache_k->size());
        ngx_md5_final(md5_buf, &md5);
        ngx_hex_dump(p, md5_buf, sizeof (md5_buf));

        cache_k->assign((char*) p, 32);

        if (CACHE[conf->cache_index]->exists(*cache_k)) {
            const cache_ele_t& cache_v = CACHE[conf->cache_index]->get(*cache_k);
            time_t now = time(NULL);
            if (difftime(now, cache_v.t) > conf->cache_expires) {
                CACHE[conf->cache_index]->erase(*cache_k);
            } else {
                ngx_response.content = cache_v.content;
                ngx_response.headers.find("Content-Type")->second = cache_v.content_type;
                ngx_response.status = cache_v.status;
                goto done;
            }
        }
    }
    if (conf->need_headers == 1) {
        get_input_headers(r, ngx_request.headers);
    }

    ngx_request.method.assign((char*) r->method_name.data, r->method_name.len);
    ngx_request.client.assign((char*) r->connection->addr_text.data, r->connection->addr_text.len);
    if (r->headers_in.user_agent->value.len > 0) {
        ngx_request.user_agent.assign((char*) r->headers_in.user_agent->value.data, r->headers_in.user_agent->value.len);
    }
    if (r->args.len > 0) {
        hi::parser_param(ngx_request.param, ngx_request.form);
    }
    if (r->headers_in.content_length_n > 0) {
        ngx_str_t body = get_input_body(r);
        if (r->headers_in.content_type->value.len < form_urlencoded_type_len
                || ngx_strncasecmp(r->headers_in.content_type->value.data, (u_char *) form_urlencoded_type,
                form_urlencoded_type_len) != 0) {
            try {
                if ((is_dir(TEMP_DIRECTORY) || mkdir(TEMP_DIRECTORY, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == 0)) {
                    ngx_http_core_loc_conf_t *clcf = (ngx_http_core_loc_conf_t *) ngx_http_get_module_loc_conf(r, ngx_http_core_module);
                    std::shared_ptr<MPFD::Parser> POSTParser(new MPFD::Parser());
                    POSTParser->SetTempDirForFileUpload(TEMP_DIRECTORY);
                    POSTParser->SetUploadedFilesStorage(MPFD::Parser::StoreUploadedFilesInFilesystem);
                    POSTParser->SetMaxCollectedDataLength(clcf->client_max_body_size);
                    POSTParser->SetContentType((char*) r->headers_in.content_type->value.data);
                    POSTParser->AcceptSomeData((char*) body.data, body.len);
                    auto fields = POSTParser->GetFieldsMap();
                    for (auto &item : fields) {
                        if (item.second->GetType() == MPFD::Field::TextType) {
                            ngx_request.form.insert(std::make_pair(item.first, item.second->GetTextTypeContent()));
                        } else {
                            std::string upload_file_name = item.second->GetFileName(), ext;
                            std::string::size_type p = upload_file_name.find_last_of(".");
                            if (p != std::string::npos) {
                                ext = upload_file_name.substr(p);
                            }
                            std::string temp_file = TEMP_DIRECTORY + ("/" + random_string(ngx_request.client + item.second->GetFileName()).append(ext));
                            rename(item.second->GetTempFileName().c_str(), temp_file.c_str());
                            ngx_request.form.insert(std::make_pair(item.first, temp_file));
                        }
                    }
                }
            } catch (MPFD::Exception& err) {
                ngx_response.content = err.GetError();
                ngx_response.status = 500;
                goto done;
            }
        } else {
            hi::parser_param(std::string((char*) body.data, body.len), ngx_request.form);
        }
    }
    if (conf->need_cookies == 1 && r->headers_in.cookies.elts != NULL && r->headers_in.cookies.nelts != 0) {
        ngx_table_elt_t ** cookies = (ngx_table_elt_t **) r->headers_in.cookies.elts;
        for (size_t i = 0; i < r->headers_in.cookies.nelts; ++i) {
            if (cookies[i]->value.data != NULL) {
                hi::parser_param(std::string((char*) cookies[i]->value.data, cookies[i]->value.len), ngx_request.cookies, ';');
            }
        }
    }
    if (conf->need_session == 1 && ngx_request.cookies.find(SESSION_ID_NAME) != ngx_request.cookies.end()) {
        if (!REDIS) {
            REDIS = std::make_shared<hi::redis>();
        }
        if (!REDIS->is_connected() && conf->redis_host.len > 0 && conf->redis_port > 0) {
            REDIS->connect((char*) conf->redis_host.data, (int) conf->redis_port);
        }
        if (REDIS->is_connected()) {
            SESSION_ID_VALUE = ngx_request.cookies[SESSION_ID_NAME ];
            if (!REDIS->exists(SESSION_ID_VALUE)) {
                REDIS->hset(SESSION_ID_VALUE, SESSION_ID_NAME, SESSION_ID_VALUE);
                REDIS->expire(SESSION_ID_VALUE, conf->session_expires);
                ngx_request.session[SESSION_ID_NAME] = SESSION_ID_VALUE;
            } else {
                REDIS->hgetall(SESSION_ID_VALUE, ngx_request.session);
            }
        }
    }
    switch (conf->app_type) {
        case application_t::__cpp__:ngx_http_hi_cpp_handler(conf, ngx_request, ngx_response);
            break;
#ifdef HTTP_HI_PYTHON
        case application_t::__python__:ngx_http_hi_python_handler(conf, ngx_request, ngx_response);
            break;
#endif
#ifdef HTTP_HI_LUA
        case application_t::__lua__:ngx_http_hi_lua_handler(conf, ngx_request, ngx_response);
            break;
#endif
#ifdef HTTP_HI_JAVA
        case application_t::__java__:ngx_http_hi_java_handler(conf, ngx_request, ngx_response);
            break;
#endif
#ifdef HTTP_HI_PHP
        case application_t::__php__:ngx_http_hi_php_handler(conf, ngx_request, ngx_response);
            break;
#endif
        default:break;
    }

    if (ngx_response.headers.count("Content-Type") > 1) {
        auto range = ngx_response.headers.equal_range("Content-Type");
        for (auto & it = range.first; it != range.second; ++it) {
            if (it->second == "text/html;charset=UTF-8") {
                ngx_response.headers.erase(it);
                break;
            }
        }
    }

    if (ngx_response.status == 200 && conf->need_cache == 1 && conf->cache_expires > 0) {
        cache_ele_t cache_v;
        cache_v.content = ngx_response.content;
        cache_v.content_type = ngx_response.headers.find("Content-Type")->second;
        cache_v.status = ngx_response.status;
        cache_v.t = time(NULL);
        CACHE[conf->cache_index]->put(*cache_k, cache_v);
    }
    if (REDIS && REDIS->is_connected() && !SESSION_ID_VALUE.empty()) {
        REDIS->hmset(SESSION_ID_VALUE, ngx_response.session);
    }

done:
    ngx_str_t response;
    response.data = (u_char*) ngx_response.content.c_str();
    response.len = ngx_response.content.size();


    ngx_buf_t *buf;
    buf = (ngx_buf_t*) ngx_pcalloc(r->pool, sizeof (ngx_buf_t));
    if (buf == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to allocate response buffer.");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    buf->pos = response.data;
    buf->last = buf->pos + response.len;
    buf->memory = 1;
    buf->last_buf = 1;

    ngx_chain_t out;
    out.buf = buf;
    out.next = NULL;

    set_output_headers(r, ngx_response.headers);
    r->headers_out.status = ngx_response.status;
    r->headers_out.content_length_n = response.len;

    ngx_int_t rc;
    rc = ngx_http_send_header(r);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    return ngx_http_output_filter(r, &out);

}

static void ngx_http_hi_body_handler(ngx_http_request_t* r) {
    ngx_http_finalize_request(r, ngx_http_hi_normal_handler(r));
}

static void get_input_headers(ngx_http_request_t* r, std::unordered_map<std::string, std::string>& input_headers) {
    ngx_table_elt_t *th;
    ngx_list_part_t *part;
    part = &r->headers_in.headers.part;
    th = (ngx_table_elt_t*) part->elts;
    ngx_uint_t i;
    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }
            part = part->next;
            th = (ngx_table_elt_t*) part->elts;
            i = 0;
        }
        input_headers[(char*) th[i].key.data] = std::move(std::string((char*) th[i].value.data, th[i].value.len));
    }
}

static void set_output_headers(ngx_http_request_t* r, std::unordered_multimap<std::string, std::string>& output_headers) {
    for (auto& item : output_headers) {
        ngx_table_elt_t * h = (ngx_table_elt_t *) ngx_list_push(&r->headers_out.headers);
        if (h) {
            h->hash = 1;
            h->key.data = (u_char*) item.first.c_str();
            h->key.len = item.first.size();
            h->value.data = (u_char*) item.second.c_str();
            h->value.len = item.second.size();
        }
    }

}

static ngx_str_t get_input_body(ngx_http_request_t *r) {
    u_char *p;
    u_char *data;
    size_t len;
    ngx_buf_t *buf, *next;
    ngx_chain_t *cl;
    ngx_str_t body = ngx_null_string;

    if (r->request_body == NULL || r->request_body->bufs == NULL) {
        return body;
    }

    if (r->request_body->temp_file) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "temp_file: %s", r->request_body->temp_file->file.name.data);
        body = r->request_body->temp_file->file.name;
        return body;
    } else {
        cl = r->request_body->bufs;
        buf = cl->buf;

        if (cl->next == NULL) {
            len = buf->last - buf->pos;
            p = (u_char*) ngx_pnalloc(r->pool, len + 1);
            if (p == NULL) {
                return body;
            }
            data = p;
            ngx_memcpy(p, buf->pos, len);
            data[len] = 0;
        } else {
            next = cl->next->buf;
            len = (buf->last - buf->pos) + (next->last - next->pos);
            p = (u_char*) ngx_pnalloc(r->pool, len + 1);
            data = p;
            if (p == NULL) {
                return body;
            }
            p = ngx_cpymem(p, buf->pos, buf->last - buf->pos);
            ngx_memcpy(p, next->pos, next->last - next->pos);
            data[len] = 0;
        }
    }

    body.len = len;
    body.data = data;
    return body;
}

static void ngx_http_hi_cpp_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    std::shared_ptr<hi::servlet> view_instance = std::move(PLUGIN[conf->module_index]->make_obj());
    if (view_instance) {
        view_instance->handler(req, res);
    }

}
#ifdef HTTP_HI_PYTHON

static void ngx_http_hi_python_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    hi::py_request py_req;
    hi::py_response py_res;
    py_req.init(&req);
    py_res.init(&res);
    if (!PYTHON) {
        PYTHON = std::make_shared<hi::pybind11_py>();
    }
    if (PYTHON) {
        PYTHON->set_req(&py_req);
        PYTHON->set_res(&py_res);
        if (conf->python_script.len > 0) {
            std::string script = std::move(std::string((char*) conf->python_script.data, conf->python_script.len));
            auto c = script.find_last_of('.');
            if (c == std::string::npos || script.substr(c + 1) != "py") {
                script.append(req.uri);
            }
            PYTHON->call_script(script);
        } else if (conf->python_content.len > 0) {
            PYTHON->call_content((char*) conf->python_content.data);
        }
    }
}
#endif
#ifdef HTTP_HI_LUA

static void ngx_http_hi_lua_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    hi::py_request py_req;
    hi::py_response py_res;
    py_req.init(&req);
    py_res.init(&res);
    if (!LUA) {
        LUA = std::make_shared<hi::lua>();
    }
    if (LUA) {
        LUA->set_req(&py_req);
        LUA->set_res(&py_res);
        if (conf->lua_script.len > 0) {
            std::string script = std::move(std::string((char*) conf->lua_script.data, conf->lua_script.len));
            auto c = script.find_last_of('.');
            if (c == std::string::npos || script.substr(c + 1) != "lua") {
                script.append(req.uri);
            }
            LUA->call_script(script);
        } else if (conf->lua_content.len > 0) {
            LUA->call_content((char*) conf->lua_content.data);
        }
    }
}
#endif
#ifdef HTTP_HI_JAVA

static void ngx_http_hi_java_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    if (java_init_handler(conf)) {

        jobject request_instance, response_instance;


        request_instance = JAVA->env->NewObject(JAVA->request, JAVA->request_ctor);
        response_instance = JAVA->env->NewObject(JAVA->response, JAVA->response_ctor);

        java_input_handler(conf, req, res, request_instance, response_instance);



        hi::java_servlet_t jtmp;
        if (JAVA_SERVLET_CACHE->exists((const char*) conf->java_servlet.data)) {
            jtmp = JAVA_SERVLET_CACHE->get((const char*) conf->java_servlet.data);
            time_t now = time(0);
            if (difftime(now, jtmp.t) > conf->java_servlet_cache_expires) {
                JAVA_SERVLET_CACHE->erase((const char*) conf->java_servlet.data);
                goto java_servlet_update;
            }
        } else {
java_servlet_update:
            jtmp.SERVLET = JAVA->env->FindClass((const char*) conf->java_servlet.data);
            if (jtmp.SERVLET == NULL)return;
            jtmp.CTOR = JAVA->env->GetMethodID(jtmp.SERVLET, "<init>", "()V");
            jtmp.HANDLER = JAVA->env->GetMethodID(jtmp.SERVLET, "handler", "(Lhi/request;Lhi/response;)V");
            jtmp.t = time(0);
            JAVA_SERVLET_CACHE->put((const char*) conf->java_servlet.data, jtmp);
        }
        jobject servlet_instance = JAVA->env->NewObject(jtmp.SERVLET, jtmp.CTOR);
        JAVA->env->CallVoidMethod(servlet_instance, jtmp.HANDLER, request_instance, response_instance);
        JAVA->env->DeleteLocalRef(servlet_instance);


        java_output_handler(conf, req, res, request_instance, response_instance);


        JAVA->env->DeleteLocalRef(request_instance);
        JAVA->env->DeleteLocalRef(response_instance);
    }

}

static void java_input_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res, jobject request_instance, jobject response_instance) {
    jstring client = JAVA->env->NewStringUTF(req.client.c_str());
    JAVA->env->SetObjectField(request_instance, JAVA->client, client);
    JAVA->env->ReleaseStringUTFChars(client, 0);
    JAVA->env->DeleteLocalRef(client);

    jstring user_agent = JAVA->env->NewStringUTF(req.user_agent.c_str());
    JAVA->env->SetObjectField(request_instance, JAVA->user_agent, user_agent);
    JAVA->env->ReleaseStringUTFChars(user_agent, 0);
    JAVA->env->DeleteLocalRef(user_agent);

    jstring method = JAVA->env->NewStringUTF(req.method.c_str());
    JAVA->env->SetObjectField(request_instance, JAVA->method, method);
    JAVA->env->ReleaseStringUTFChars(method, 0);
    JAVA->env->DeleteLocalRef(method);

    jstring uri = JAVA->env->NewStringUTF(req.uri.c_str());
    JAVA->env->SetObjectField(request_instance, JAVA->uri, uri);
    JAVA->env->ReleaseStringUTFChars(uri, 0);
    JAVA->env->DeleteLocalRef(uri);

    jstring param = JAVA->env->NewStringUTF(req.param.c_str());
    JAVA->env->SetObjectField(request_instance, JAVA->param, param);
    JAVA->env->ReleaseStringUTFChars(param, 0);
    JAVA->env->DeleteLocalRef(param);

    if (conf->need_headers == 1) {
        jobject req_headers = JAVA->env->GetObjectField(request_instance, JAVA->req_headers);
        for (auto& item : req.headers) {
            jstring k = JAVA->env->NewStringUTF(item.first.c_str())
                    , v = JAVA->env->NewStringUTF(item.second.c_str());
            JAVA->env->CallObjectMethod(req_headers, JAVA->hashmap_put, k, v);
            JAVA->env->ReleaseStringUTFChars(k, 0);
            JAVA->env->ReleaseStringUTFChars(v, 0);
            JAVA->env->DeleteLocalRef(k);
            JAVA->env->DeleteLocalRef(v);
        }
        JAVA->env->DeleteLocalRef(req_headers);
    }

    jobject req_form = JAVA->env->GetObjectField(request_instance, JAVA->form);
    for (auto& item : req.form) {
        jstring k = JAVA->env->NewStringUTF(item.first.c_str())
                , v = JAVA->env->NewStringUTF(item.second.c_str());
        JAVA->env->CallObjectMethod(req_form, JAVA->hashmap_put, k, v);
        JAVA->env->ReleaseStringUTFChars(k, 0);
        JAVA->env->ReleaseStringUTFChars(v, 0);
        JAVA->env->DeleteLocalRef(k);
        JAVA->env->DeleteLocalRef(v);
    }
    JAVA->env->DeleteLocalRef(req_form);

    if (conf->need_cookies == 1) {
        jobject req_cookies = JAVA->env->GetObjectField(request_instance, JAVA->cookies);
        for (auto& item : req.cookies) {
            jstring k = JAVA->env->NewStringUTF(item.first.c_str())
                    , v = JAVA->env->NewStringUTF(item.second.c_str());
            JAVA->env->CallObjectMethod(req_cookies, JAVA->hashmap_put, k, v);
            JAVA->env->ReleaseStringUTFChars(k, 0);
            JAVA->env->ReleaseStringUTFChars(v, 0);
            JAVA->env->DeleteLocalRef(k);
            JAVA->env->DeleteLocalRef(v);
        }
        JAVA->env->DeleteLocalRef(req_cookies);
    }

    if (conf->need_session == 1) {
        jobject req_session = JAVA->env->GetObjectField(request_instance, JAVA->req_session);
        for (auto& item : req.session) {
            jstring k = JAVA->env->NewStringUTF(item.first.c_str())
                    , v = JAVA->env->NewStringUTF(item.second.c_str());
            JAVA->env->CallObjectMethod(req_session, JAVA->hashmap_put, k, v);
            JAVA->env->ReleaseStringUTFChars(k, 0);
            JAVA->env->ReleaseStringUTFChars(v, 0);
            JAVA->env->DeleteLocalRef(k);
            JAVA->env->DeleteLocalRef(v);
        }
        JAVA->env->DeleteLocalRef(req_session);
    }

}

static void java_output_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res, jobject request_instance, jobject response_instance) {
    jobject res_headers = JAVA->env->GetObjectField(response_instance, JAVA->res_headers);
    jobject keyset = JAVA->env->CallObjectMethod(res_headers, JAVA->hashmap_keyset);
    jobject iterator = JAVA->env->CallObjectMethod(keyset, JAVA->set_iterator);
    while ((bool)JAVA->env->CallBooleanMethod(iterator, JAVA->hasnext)) {
        jstring k = (jstring) JAVA->env->CallObjectMethod(iterator, JAVA->next);
        jobject item = JAVA->env->CallObjectMethod(res_headers, JAVA->hashmap_get, k);
        jobject jterator = JAVA->env->CallObjectMethod(item, JAVA->arraylist_iterator);
        const char * kstr = JAVA->env->GetStringUTFChars(k, NULL);
        while ((bool)JAVA->env->CallBooleanMethod(jterator, JAVA->hasnext)) {
            jstring v = (jstring) JAVA->env->CallObjectMethod(jterator, JAVA->next);
            const char* vstr = JAVA->env->GetStringUTFChars(v, NULL);
            res.headers.insert(std::make_pair(kstr, vstr));
            JAVA->env->ReleaseStringUTFChars(v, vstr);
            JAVA->env->DeleteLocalRef(v);
        }
        JAVA->env->ReleaseStringUTFChars(k, kstr);
        JAVA->env->DeleteLocalRef(k);
        JAVA->env->DeleteLocalRef(item);
        JAVA->env->DeleteLocalRef(jterator);
    }
    JAVA->env->DeleteLocalRef(res_headers);
    JAVA->env->DeleteLocalRef(keyset);
    JAVA->env->DeleteLocalRef(iterator);

    if (conf->need_session == 1) {
        jobject res_session = JAVA->env->GetObjectField(response_instance, JAVA->res_session);
        keyset = JAVA->env->CallObjectMethod(res_session, JAVA->hashmap_keyset);
        iterator = JAVA->env->CallObjectMethod(keyset, JAVA->set_iterator);
        while ((bool)JAVA->env->CallBooleanMethod(iterator, JAVA->hasnext)) {
            jstring k = (jstring) JAVA->env->CallObjectMethod(iterator, JAVA->next);
            jstring v = (jstring) JAVA->env->CallObjectMethod(res_session, JAVA->hashmap_get, k);
            const char * kstr = JAVA->env->GetStringUTFChars(k, NULL),
                    * vstr = JAVA->env->GetStringUTFChars(v, NULL);
            res.session[kstr] = vstr;
            JAVA->env->ReleaseStringUTFChars(k, kstr);
            JAVA->env->DeleteLocalRef(k);
            JAVA->env->ReleaseStringUTFChars(v, vstr);
            JAVA->env->DeleteLocalRef(v);
        }
        JAVA->env->DeleteLocalRef(res_session);
        JAVA->env->DeleteLocalRef(keyset);
        JAVA->env->DeleteLocalRef(iterator);
    }

    res.status = JAVA->env->GetIntField(response_instance, JAVA->status);
    jstring content = (jstring) JAVA->env->GetObjectField(response_instance, JAVA->content);
    const char* contentstr = JAVA->env->GetStringUTFChars(content, NULL);
    res.content = contentstr;
    JAVA->env->ReleaseStringUTFChars(content, contentstr);
    JAVA->env->DeleteLocalRef(content);
}

static bool java_init_handler(ngx_http_hi_loc_conf_t * conf) {
    if (JAVA_IS_READY)return JAVA_IS_READY;
    if (!JAVA) {
        JAVA = std::make_shared<hi::java>((char*) conf->java_classpath.data, (char*) conf->java_options.data, conf->java_version);
        if (JAVA->is_ok()) {
            JAVA->request = JAVA->env->FindClass("hi/request");
            if (JAVA->request != NULL) {
                JAVA->request_ctor = JAVA->env->GetMethodID(JAVA->request, "<init>", "()V");
                JAVA->client = JAVA->env->GetFieldID(JAVA->request, "client", "Ljava/lang/String;");
                JAVA->user_agent = JAVA->env->GetFieldID(JAVA->request, "user_agent", "Ljava/lang/String;");
                JAVA->method = JAVA->env->GetFieldID(JAVA->request, "method", "Ljava/lang/String;");
                JAVA->uri = JAVA->env->GetFieldID(JAVA->request, "uri", "Ljava/lang/String;");
                JAVA->param = JAVA->env->GetFieldID(JAVA->request, "param", "Ljava/lang/String;");
                JAVA->req_headers = JAVA->env->GetFieldID(JAVA->request, "headers", "Ljava/util/HashMap;");
                JAVA->form = JAVA->env->GetFieldID(JAVA->request, "form", "Ljava/util/HashMap;");
                JAVA->cookies = JAVA->env->GetFieldID(JAVA->request, "cookies", "Ljava/util/HashMap;");
                JAVA->req_session = JAVA->env->GetFieldID(JAVA->request, "session", "Ljava/util/HashMap;");


                JAVA->response = JAVA->env->FindClass("hi/response");
                if (JAVA->response != NULL) {
                    JAVA->response_ctor = JAVA->env->GetMethodID(JAVA->response, "<init>", "()V");
                    JAVA->status = JAVA->env->GetFieldID(JAVA->response, "status", "I");
                    JAVA->content = JAVA->env->GetFieldID(JAVA->response, "content", "Ljava/lang/String;");
                    JAVA->res_headers = JAVA->env->GetFieldID(JAVA->response, "headers", "Ljava/util/HashMap;");
                    JAVA->res_session = JAVA->env->GetFieldID(JAVA->response, "session", "Ljava/util/HashMap;");

                    JAVA->hashmap = JAVA->env->FindClass("java/util/HashMap");
                    JAVA->hashmap_put = JAVA->env->GetMethodID(JAVA->hashmap, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
                    JAVA->hashmap_get = JAVA->env->GetMethodID(JAVA->hashmap, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
                    JAVA->hashmap_keyset = JAVA->env->GetMethodID(JAVA->hashmap, "keySet", "()Ljava/util/Set;");

                    JAVA->arraylist = JAVA->env->FindClass("java/util/ArrayList");
                    JAVA->arraylist_get = JAVA->env->GetMethodID(JAVA->arraylist, "get", "(I)Ljava/lang/Object;");
                    JAVA->arraylist_size = JAVA->env->GetMethodID(JAVA->arraylist, "size", "()I");
                    JAVA->arraylist_iterator = JAVA->env->GetMethodID(JAVA->arraylist, "iterator", "()Ljava/util/Iterator;");

                    JAVA->iterator = JAVA->env->FindClass("java/util/Iterator");
                    JAVA->hasnext = JAVA->env->GetMethodID(JAVA->iterator, "hasNext", "()Z");
                    JAVA->next = JAVA->env->GetMethodID(JAVA->iterator, "next", "()Ljava/lang/Object;");

                    JAVA->set = JAVA->env->FindClass("java/util/Set");
                    JAVA->set_iterator = JAVA->env->GetMethodID(JAVA->set, "iterator", "()Ljava/util/Iterator;");
                    JAVA_IS_READY = true;
                }
            }
        }
    }
    return JAVA_IS_READY;
}
#endif

static std::string md5(const std::string& str) {
    unsigned char digest[16] = {0};
    MD5_CTX ctx;
    MD5_Init(&ctx);
    MD5_Update(&ctx, str.c_str(), str.size());
    MD5_Final(digest, &ctx);

    unsigned char tmp[32] = {0}, *dst = &tmp[0], *src = &digest[0];
    unsigned char hex[] = "0123456789abcdef";
    int len = 16;
    while (len--) {
        *dst++ = hex[*src >> 4];
        *dst++ = hex[*src++ & 0xf];
    }

    return std::string((char*) tmp, 32);
}

static std::string random_string(const std::string& s) {
    time_t now = time(NULL);
    char* now_str = ctime(&now);
    return md5(s + now_str);
}

static bool is_dir(const std::string& s) {
    struct stat st;
    return stat(s.c_str(), &st) >= 0 && S_ISDIR(st.st_mode);
}

#ifdef HTTP_HI_PHP

static void ngx_http_hi_php_handler(ngx_http_hi_loc_conf_t * conf, hi::request& req, hi::response& res) {
    std::string script = std::move(std::string((char*) conf->php_script.data, conf->php_script.len));
    auto c = script.find_last_of('.');
    if (c == std::string::npos || script.substr(c + 1) != "php") {
        script.append(req.uri);
    }
    if (access(script.c_str(), F_OK) == 0) {
        zend_first_try
                {
            PHP->include(script.c_str());
            const char *request = "\\hi\\request", *response = "\\hi\\response", *handler = "handler";
            php::Object php_req = php::newObject(request), php_res = php::newObject(response);
            if (!php_req.isNull()&&!php_res.isNull()) {
                php_req.set("client", php::Variant(req.client));
                php_req.set("method", php::Variant(req.method));
                php_req.set("user_agent", php::Variant(req.user_agent));
                php_req.set("param", php::Variant(req.param));
                php_req.set("uri", php::Variant(req.uri));

                php::Array php_req_headers, php_req_form, php_req_cookies, php_req_session;
                for (auto & i : req.headers) {
                    php_req_headers.set(i.first.c_str(), php::Variant(i.second));
                }
                for (auto & i : req.form) {
                    php_req_form.set(i.first.c_str(), php::Variant(i.second));
                }
                for (auto & i : req.cookies) {
                    php_req_cookies.set(i.first.c_str(), php::Variant(i.second));
                }
                for (auto & i : req.session) {
                    php_req_session.set(i.first.c_str(), php::Variant(i.second));
                }
                php_req.set("headers", php_req_headers);
                php_req.set("form", php_req_form);
                php_req.set("cookies", php_req_cookies);
                php_req.set("session", php_req_session);




                auto p = script.find_last_of('/'), q = script.find_last_of('.');

                std::string class_name = std::move(script.substr(p + 1, q - 1 - p));


                php::Object servlet = php::newObject(class_name.c_str());


                if (!servlet.isNull() && servlet.methodExists(handler)) {
                    servlet.exec(handler, php_req, php_res);
                    php::Array res_headers = php_res.get("headers"), res_session = php_res.get("session");


                    for (auto i = res_headers.begin(); i != res_headers.end(); i++) {
                        auto v = i.value();
                        if (v.isArray()) {
                            php::Array arr(v);
                            for (size_t j = 0; j < arr.count(); j++) {
                                res.headers.insert(std::move(std::make_pair(i.key().toString(), arr[j].toString())));
                            }
                            arr.clean();
                        } else {
                            res.headers.insert(std::move(std::make_pair(i.key().toString(), i.value().toString())));
                        }
                    }
                    for (auto i = res_session.begin(); i != res_session.end(); i++) {
                        res.session.insert(std::move(std::make_pair(i.key().toString(), i.value().toString())));
                    }

                    res.content = std::move(php_res.get("content").toString());

                    res.status = std::move(php_res.get("status")).toInt();


                    php_req_headers.clean();
                    php_req_form.clean();
                    php_req_cookies.clean();
                    php_req_session.clean();

                    res_headers.clean();
                    res_session.clean();

                    return;
                }
            }}zend_catch{
            res.content = std::move("<p style='text-align:center;margin:100px;'>PHP Throw Exception</p>");
            res.status = 500;}zend_end_try();
    }
}
#endif


#endif