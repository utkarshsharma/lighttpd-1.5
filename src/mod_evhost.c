#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "plugin.h"
#include "log.h"
#include "response.h"
#include "file_cache.h"

typedef struct {
	/* unparsed pieces */
	buffer *path_pieces_raw;
	
	/* pieces for path creation */
	size_t len;
	buffer **path_pieces;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	buffer *tmp_buf;

	plugin_config **config_storage;
	plugin_config conf; 
} plugin_data;

INIT_FUNC(mod_evhost_init) {
	plugin_data *p;
	
	p = calloc(1, sizeof(*p));
	
	p->tmp_buf = buffer_init();

	return p;
}

FREE_FUNC(mod_evhost_free) {
	plugin_data *p = p_d;
	
	UNUSED(srv);

	if (!p) return HANDLER_GO_ON;
	
	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];
			
			if(s->path_pieces) {
				for (i = 0; i < s->len; i++) {
					buffer_free(s->path_pieces[i]);
				}
				
				free(s->path_pieces);
			}
			
			buffer_free(s->path_pieces_raw);
			
			free(s);
		}
		free(p->config_storage);
	}
	
	buffer_free(p->tmp_buf);

	free(p);

	return HANDLER_GO_ON;
}

static void mod_evhost_parse_pattern(plugin_config *s) {
	char *ptr = s->path_pieces_raw->ptr,*pos;
	
	s->path_pieces = NULL;
	
	for(pos=ptr;*ptr;ptr++) {
		if(*ptr == '%') {
			s->path_pieces = realloc(s->path_pieces,(s->len+2) * sizeof(*s->path_pieces));
			s->path_pieces[s->len] = buffer_init();
			s->path_pieces[s->len+1] = buffer_init();
			
			buffer_copy_string_len(s->path_pieces[s->len],pos,ptr-pos);
			pos = ptr + 2;
			
			buffer_copy_string_len(s->path_pieces[s->len+1],ptr++,2);
			
			s->len += 2;
		}
	}
	
	if(*pos != '\0') {
		s->path_pieces = realloc(s->path_pieces,(s->len+1) * sizeof(*s->path_pieces));
		s->path_pieces[s->len] = buffer_init();
		
		buffer_append_memory(s->path_pieces[s->len],pos,ptr-pos);
		
		s->len += 1;
	}
}

SETDEFAULTS_FUNC(mod_evhost_set_defaults) {
	plugin_data *p = p_d;
	size_t i;
	
	/**
	 * 
	 * #
	 * # define a pattern for the host url finding
	 * # %% => % sign
	 * # %0 => domain name + tld
	 * # %1 => tld
	 * # %2 => domain name without tld
	 * # %3 => subdomain 1 name
	 * # %4 => subdomain 2 name
	 * #
	 * evhost.path-pattern = "/home/ckruse/dev/www/%3/htdocs/"
	 * 
	 */
	
	config_values_t cv[] = { 
		{ "evhost.path-pattern",            NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
		{ NULL,                             NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};
	
	if (!p) return HANDLER_ERROR;
	
	p->config_storage = malloc(srv->config_context->used * sizeof(specific_config *));
	
	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;
		
		s = calloc(1, sizeof(plugin_config));
		s->path_pieces_raw = buffer_init();
		s->path_pieces     = NULL;
		s->len             = 0;
	
		cv[0].destination = s->path_pieces_raw;
		
		p->config_storage[i] = s;
		
		if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value,  cv)) {
			return HANDLER_ERROR;
		}
		
		if (s->path_pieces_raw->used != 0) {
			mod_evhost_parse_pattern(s);
		}
	}
	
	return HANDLER_GO_ON;
}

/**
 * assign the different parts of the domain to array-indezes
 * - %0 - full hostname (authority w/o port)
 * - %1 - tld
 * - %2 - domain.tld
 * - %3 - 
 */

static int mod_evhost_parse_host(connection *con,array *host) {
	/* con->uri.authority->used is always > 0 if we come here */
	register char *ptr = con->uri.authority->ptr + con->uri.authority->used - 1;
	char *colon = ptr; /* needed to filter out the colon (if exists) */
	int first = 1;
	data_string *ds;
	int i;
	
	/* first, find the domain + tld */
	for(;ptr > con->uri.authority->ptr;ptr--) {
		if(*ptr == '.') {
			if(first) first = 0;
			else      break;
		} else if(*ptr == ':') {
			colon = ptr;
			first = 1;
		}
	}
	
	ds = data_string_init();
	buffer_copy_string(ds->key,"%0");
	
	/* if we stopped at a dot, skip the dot */
	if (*ptr == '.') ptr++;
	buffer_copy_string_len(ds->value, ptr, colon-ptr);
	
	array_insert_unique(host,(data_unset *)ds);
	
	/* if the : is not the start of the authority, go on parsing the hostname */
	
	if (colon != con->uri.authority->ptr) {
		for(ptr = colon - 1, i = 1; ptr > con->uri.authority->ptr; ptr--) {
			if(*ptr == '.') {
				if (ptr != colon - 1) {
					/* is something between the dots */
					ds = data_string_init();
					buffer_copy_string(ds->key,"%");
					buffer_append_long(ds->key, i++);
					buffer_copy_string_len(ds->value,ptr+1,colon-ptr-1);
					
					array_insert_unique(host,(data_unset *)ds);
				}
				colon = ptr;
			}
		}
		
		/* if the . is not the first charactor of the hostname */
		if (colon != ptr) {
			ds = data_string_init();
			buffer_copy_string(ds->key,"%");
			buffer_append_long(ds->key, i++);
			buffer_copy_string_len(ds->value,ptr,colon-ptr);
			
			array_insert_unique(host,(data_unset *)ds);
		}
	}
	
	return 0;
}

#define PATCH(x) \
	p->conf.x = s->x;
static int mod_evhost_patch_connection(server *srv, connection *con, plugin_data *p, const char *stage, size_t stage_len) {
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
			
			if (buffer_is_equal_string(du->key, CONST_STR_LEN("evhost.path-pattern"))) {
				PATCH(path_pieces);
				PATCH(len);
			}
		}
	}
	
	return 0;
}

static int mod_evhost_setup_connection(server *srv, connection *con, plugin_data *p) {
	plugin_config *s = p->config_storage[0];
	UNUSED(srv);
	UNUSED(con);
		
	PATCH(path_pieces);
	PATCH(len);
	
	return 0;
}
#undef PATCH


static handler_t mod_evhost_uri_handler(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;
	size_t i;
	array *parsed_host;
	register char *ptr;
	int not_good = 0;
	
	/* not authority set */
	if (con->uri.authority->used == 0) return HANDLER_GO_ON;
	
	mod_evhost_setup_connection(srv, con, p);
	for (i = 0; i < srv->config_patches->used; i++) {
		buffer *patch = srv->config_patches->ptr[i];
		
		mod_evhost_patch_connection(srv, con, p, CONST_BUF_LEN(patch));
	}
	
	parsed_host = array_init();
	
	mod_evhost_parse_host(con, parsed_host);
	
	/* build document-root */
	buffer_reset(p->tmp_buf);
	
	for (i = 0; i < p->conf.len; i++) {
		ptr = p->conf.path_pieces[i]->ptr;
		if (*ptr == '%') {
			data_string *ds;
			
			if (*(ptr+1) == '%') {
				/* %% */
				BUFFER_APPEND_STRING_CONST(p->tmp_buf,"%");
			} else if (NULL != (ds = (data_string *)array_get_element(parsed_host,p->conf.path_pieces[i]->ptr))) {
				if (ds->value->used) {
					buffer_append_string_buffer(p->tmp_buf,ds->value);
				}
			} else {
				/* unhandled %-sequence */
			}
		} else {
			buffer_append_string_buffer(p->tmp_buf,p->conf.path_pieces[i]);
		}
	}
	
	BUFFER_APPEND_SLASH(p->tmp_buf);
	
	array_free(parsed_host);
	
	if (HANDLER_GO_ON != file_cache_get_entry(srv, con, p->tmp_buf, &(con->fce))) {
		log_error_write(srv, __FILE__, __LINE__, "sb", strerror(errno), p->tmp_buf);
		not_good = 1;
	} else if(!S_ISDIR(con->fce->st.st_mode)) {
		log_error_write(srv, __FILE__, __LINE__, "sb", "not a directory:", p->tmp_buf);
		not_good = 1;
	}
	
	if (!not_good) {
		buffer_copy_string_buffer(con->physical.doc_root, p->tmp_buf);
	}
	
	return HANDLER_GO_ON;
}

int mod_evhost_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name                    = buffer_init_string("evhost");
	p->init                    = mod_evhost_init;
	p->set_defaults            = mod_evhost_set_defaults;
	p->handle_docroot          = mod_evhost_uri_handler;
	p->cleanup                 = mod_evhost_free;
	
	p->data                    = NULL;
	
	return 0;
}

/* eof */
