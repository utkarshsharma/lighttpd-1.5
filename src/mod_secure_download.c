#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"
#include "log.h"
#include "buffer.h"

#include "plugin.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef USE_OPENSSL
# include <openssl/md5.h>
#else
# include "md5_global.h"
# include "md5.h"
#endif

#define HASHLEN 16
typedef unsigned char HASH[HASHLEN];
#define HASHHEXLEN 32
typedef char HASHHEX[HASHHEXLEN+1];
#ifdef USE_OPENSSL
#define IN const
#else
#define IN 
#endif
#define OUT


/* plugin config for all request/connections */

typedef struct {
	buffer *doc_root;
	buffer *secret;
	buffer *uri_prefix;
	
	time_t timeout;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	
	buffer *md5;
	
	plugin_config **config_storage;
	
	plugin_config conf; 
} plugin_data;

/* init the plugin data */
INIT_FUNC(mod_secdownload_init) {
	plugin_data *p;
	
	p = calloc(1, sizeof(*p));
	
	p->md5 = buffer_init();
	
	return p;
}

/* detroy the plugin data */
FREE_FUNC(mod_secdownload_free) {
	plugin_data *p = p_d;
	UNUSED(srv);
	
	if (!p) return HANDLER_GO_ON;
	
	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];
			
			buffer_free(s->secret);
			buffer_free(s->doc_root);
			buffer_free(s->uri_prefix);
			
			free(s);
		}
		free(p->config_storage);
	}
	
	buffer_free(p->md5);
	
	free(p);
	
	return HANDLER_GO_ON;
}

/* handle plugin config and check values */

SETDEFAULTS_FUNC(mod_secdownload_set_defaults) {
	plugin_data *p = p_d;
	size_t i = 0;
	
	config_values_t cv[] = { 
		{ "secdownload.secret",            NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ "secdownload.document-root",     NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 1 */
		{ "secdownload.uri-prefix",        NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 2 */
		{ "secdownload.timeout",           NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },        /* 3 */
		{ NULL,                            NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};
	
	if (!p) return HANDLER_ERROR;
	
	p->config_storage = malloc(srv->config_context->used * sizeof(specific_config *));
	
	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;
		
		s = malloc(sizeof(plugin_config));
		s->secret        = buffer_init();
		s->doc_root      = buffer_init();
		s->uri_prefix    = buffer_init();
		s->timeout = 0;
		
		/* set global default */
		if (i == 0) {
			s->timeout = 60;
			buffer_copy_string(s->uri_prefix, "/");
		}
		
		cv[0].destination = s->secret;
		cv[1].destination = s->doc_root;
		cv[2].destination = s->uri_prefix;
		cv[3].destination = &(s->timeout);
		
		p->config_storage[i] = s;
	
		if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value, cv)) {
			return HANDLER_ERROR;
		}
	}
	
	return HANDLER_GO_ON;
}

/**
 * checks if the supplied string is a MD5 string
 * 
 * @param str a possible MD5 string
 * @return if the supplied string is a valid MD5 string 1 is returned otherwise 0
 */

int is_hex_len(const char *str, size_t len) {
	size_t i;
	
	if (NULL == str) return 0;
	
	for (i = 0; i < len && *str; i++, str++) {
		/* illegal characters */
		if (!((*str >= '0' && *str <= '9') ||
		      (*str >= 'a' && *str <= 'f') ||
		      (*str >= 'A' && *str <= 'F')) 
		    ) {
			return 0;
		}
	}
	
	return i == len;
}

#define PATCH(x) \
	p->conf.x = s->x;
static int mod_secdownload_patch_connection(server *srv, connection *con, plugin_data *p, const char *stage, size_t stage_len) {
	size_t i, j;
	
	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		plugin_config *s = p->config_storage[i];
		
		/* not our stage */
		if (!buffer_is_equal_string(dc->comp_key, stage, stage_len)) continue;
		
		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;
		
		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];
			
			if (buffer_is_equal_string(du->key, CONST_STR_LEN("secdownload.secret"))) {
				PATCH(secret);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("secdownload.document-root"))) {
				PATCH(doc_root);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("secdownload.uri-prefix"))) {
				PATCH(uri_prefix);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("secdownload.timeout"))) {
				PATCH(timeout);
			}
		}
	}
	
	return 0;
}

static int mod_secdownload_setup_connection(server *srv, connection *con, plugin_data *p) {
	plugin_config *s = p->config_storage[0];
	UNUSED(srv);
	UNUSED(con);

	PATCH(secret);
	PATCH(doc_root);
	PATCH(uri_prefix);
	PATCH(timeout);
	
	return 0;
}
#undef PATCH


URIHANDLER_FUNC(mod_secdownload_uri_handler) {
	plugin_data *p = p_d;
	MD5_CTX Md5Ctx;
	HASH HA1;
	const char *rel_uri, *ts_str, *md5_str;
	time_t ts = 0;
	size_t i;
	
	if (con->uri.path->used == 0) return HANDLER_GO_ON;
	
	mod_secdownload_setup_connection(srv, con, p);
	for (i = 0; i < srv->config_patches->used; i++) {
		buffer *patch = srv->config_patches->ptr[i];
		
		mod_secdownload_patch_connection(srv, con, p, CONST_BUF_LEN(patch));
	}
	
	if (buffer_is_empty(p->conf.secret)) {
		log_error_write(srv, __FILE__, __LINE__, "s",
				"secdownload.secret has to be set");
		return HANDLER_ERROR;
	}
	
	if (buffer_is_empty(p->conf.doc_root)) {
		log_error_write(srv, __FILE__, __LINE__, "s",
				"secdownload.document-root has to be set");
		return HANDLER_ERROR;
	}
	
	/* 
	 *  /<uri-prefix>[a-f0-9]{32}/[a-f0-9]{8}/<rel-path>
	 */
	
	if (0 != strncmp(con->uri.path->ptr, p->conf.uri_prefix->ptr, p->conf.uri_prefix->used - 1)) return HANDLER_GO_ON;
	
	md5_str = con->uri.path->ptr + p->conf.uri_prefix->used - 1;
	
	if (!is_hex_len(md5_str, 32)) return HANDLER_GO_ON;
	if (*(md5_str + 32) != '/') return HANDLER_GO_ON;
	
	ts_str = md5_str + 32 + 1;
	
	if (!is_hex_len(ts_str, 8)) return HANDLER_GO_ON;
	if (*(ts_str + 8) != '/') return HANDLER_GO_ON;
	
	for (i = 0; i < 8; i++) {
		ts = (ts << 4) + hex2int(*(ts_str + i));
	}
	
	/* timed-out */
	if (srv->cur_ts - ts > p->conf.timeout || 
	    srv->cur_ts - ts < -p->conf.timeout) {
		con->http_status = 408;
		
		return HANDLER_FINISHED;
	}
	
	rel_uri = ts_str + 8;
	
	/* checking MD5 
	 * 
	 * <secret><rel-path><timestamp-hex>
	 */
	
	buffer_copy_string_buffer(p->md5, p->conf.secret);
	buffer_append_string(p->md5, rel_uri);
	buffer_append_string_len(p->md5, ts_str, 8);
	
	MD5_Init(&Md5Ctx);
	MD5_Update(&Md5Ctx, (unsigned char *)p->md5->ptr, p->md5->used - 1);
	MD5_Final(HA1, &Md5Ctx);
	
	buffer_copy_string_hex(p->md5, (char *)HA1, 16);
	
	if (0 != strncmp(md5_str, p->md5->ptr, 32)) {
		con->http_status = 403;
		
		log_error_write(srv, __FILE__, __LINE__, "sss", 
				"md5 invalid:",
				md5_str, p->md5->ptr);
		
		return HANDLER_FINISHED;
	}
	
	/* starting with the last / we should have relative-path to the docroot
	 */
	
	buffer_copy_string_buffer(con->physical.path, p->conf.doc_root);
	buffer_append_string(con->physical.path, rel_uri);
	
	return HANDLER_COMEBACK;
}

/* this function is called at dlopen() time and inits the callbacks */

int mod_secdownload_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = buffer_init_string("secdownload");
	
	p->init        = mod_secdownload_init;
	p->handle_uri_clean  = mod_secdownload_uri_handler;
	p->set_defaults  = mod_secdownload_set_defaults;
	p->cleanup     = mod_secdownload_free;
	
	p->data        = NULL;
	
	return 0;
}