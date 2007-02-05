#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <assert.h>
#include <signal.h>

#include "buffer.h"
#include "server.h"
#include "keyvalue.h"
#include "log.h"

#include "fdevent.h"
#include "connections.h"
#include "response.h"
#include "joblist.h"
#include "status_counter.h"

#include "plugin.h"

#include "inet_ntop_cache.h"
#include "stat_cache.h"

#ifdef HAVE_FASTCGI_FASTCGI_H
#include <fastcgi/fastcgi.h>
#else
#ifdef HAVE_FASTCGI_H
#include <fastcgi.h>
#else
#include "fastcgi.h"
#endif
#endif /* HAVE_FASTCGI_FASTCGI_H */
#include <stdio.h>

#ifdef HAVE_SYS_FILIO_H
# include <sys/filio.h>
#endif

#include "sys-socket.h"
#include "sys-files.h"
#include "sys-strings.h"
#include "sys-process.h"

#include "http_resp.h"

#ifndef UNIX_PATH_MAX
# define UNIX_PATH_MAX 108
#endif

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

/*
 *
 * TODO:
 *
 * - add timeout for a connect to a non-fastcgi process
 *   (use state_timestamp + state)
 *
 */

typedef struct fcgi_proc {
	size_t id; /* id will be between 1 and max_procs */
	buffer *unixsocket; /* config.socket + "-" + id */
	unsigned port;  /* config.port + pno */

	buffer *connection_name; /* either tcp:<host>:<port> or unix:<socket> for debuggin purposes */

	pid_t pid;   /* PID of the spawned process (0 if not spawned locally) */


	size_t load; /* number of requests waiting on this process */

	time_t last_used; /* see idle_timeout */
	size_t requests;  /* see max_requests */
	struct fcgi_proc *prev, *next; /* see first */

	time_t disabled_until; /* this proc is disabled until, use something else until than */

	int is_local;

	enum {
		PROC_STATE_UNSET,    /* init-phase */
		PROC_STATE_RUNNING,  /* alive */
		PROC_STATE_OVERLOADED, /* listen-queue is full,
					  don't send something to this proc for the next 2 seconds */
		PROC_STATE_DIED_WAIT_FOR_PID, /* */
		PROC_STATE_DIED,     /* marked as dead, should be restarted */
		PROC_STATE_KILLED    /* was killed as we don't have the load anymore */
	} state;
} fcgi_proc;

typedef struct {
	/* the key that is used to reference this value */
	buffer *id;

	/* list of processes handling this extension
	 * sorted by lowest load
	 *
	 * whenever a job is done move it up in the list
	 * until it is sorted, move it down as soon as the
	 * job is started
	 */
	fcgi_proc *first;
	fcgi_proc *unused_procs;

	/*
	 * spawn at least min_procs, at max_procs.
	 *
	 * as soon as the load of the first entry
	 * is max_load_per_proc we spawn a new one
	 * and add it to the first entry and give it
	 * the load
	 *
	 */

	unsigned short min_procs;
	unsigned short max_procs;
	size_t num_procs;    /* how many procs are started */
	size_t active_procs; /* how many of them are really running */

	unsigned short max_load_per_proc;

	/*
	 * kick the process from the list if it was not
	 * used for idle_timeout until min_procs is
	 * reached. this helps to get the processlist
	 * small again we had a small peak load.
	 *
	 */

	unsigned short idle_timeout;

	/*
	 * time after a disabled remote connection is tried to be re-enabled
	 *
	 *
	 */

	unsigned short disable_time;

	/*
	 * same fastcgi processes get a little bit larger
	 * than wanted. max_requests_per_proc kills a
	 * process after a number of handled requests.
	 *
	 */
	size_t max_requests_per_proc;


	/* config */

	/*
	 * host:port
	 *
	 * if host is one of the local IP adresses the
	 * whole connection is local
	 *
	 * if tcp/ip should be used host AND port have
	 * to be specified
	 *
	 */
	buffer *host;
	unsigned short port;

	/*
	 * Unix Domain Socket
	 *
	 * instead of TCP/IP we can use Unix Domain Sockets
	 * - more secure (you have fileperms to play with)
	 * - more control (on locally)
	 * - more speed (no extra overhead)
	 */
	buffer *unixsocket;

	/* if socket is local we can start the fastcgi
	 * process ourself
	 *
	 * bin-path is the path to the binary
	 *
	 * check min_procs and max_procs for the number
	 * of process to start-up
	 */
	buffer *bin_path;

	/* bin-path is set bin-environment is taken to
	 * create the environement before starting the
	 * FastCGI process
	 *
	 */
	array *bin_env;

	array *bin_env_copy;

	/*
	 * docroot-translation between URL->phys and the
	 * remote host
	 *
	 * reasons:
	 * - different dir-layout if remote
	 * - chroot if local
	 *
	 */
	buffer *docroot;

	/*
	 * fastcgi-mode:
	 * - responser
	 * - authorizer
	 *
	 */
	unsigned short mode;

	/*
	 * check_local tell you if the phys file is stat()ed
	 * or not. FastCGI doesn't care if the service is
	 * remote. If the web-server side doesn't contain
	 * the fastcgi-files we should not stat() for them
	 * and say '404 not found'.
	 */
	unsigned short check_local;

	/*
	 * append PATH_INFO to SCRIPT_FILENAME
	 *
	 * php needs this if cgi.fix_pathinfo is provied
	 *
	 */

	unsigned short break_scriptfilename_for_php;

	/*
	 * If the backend includes X-LIGHTTPD-send-file in the response
	 * we use the value as filename and ignore the content.
	 *
	 */
	unsigned short allow_xsendfile;

	ssize_t load; /* replace by host->load */

	size_t max_id; /* corresponds most of the time to
	num_procs.

	only if a process is killed max_id waits for the process itself
	to die and decrements its afterwards */

	buffer *strip_request_uri;
} fcgi_extension_host;

/*
 * one extension can have multiple hosts assigned
 * one host can spawn additional processes on the same
 *   socket (if we control it)
 *
 * ext -> host -> procs
 *    1:n     1:n
 *
 * if the fastcgi process is remote that whole goes down
 * to
 *
 * ext -> host -> procs
 *    1:n     1:1
 *
 * in case of PHP and FCGI_CHILDREN we have again a procs
 * but we don't control it directly.
 *
 */

typedef struct {
	buffer *key; /* like .php */

	int note_is_sent;

	fcgi_extension_host **hosts;

	size_t used;
	size_t size;
} fcgi_extension;

typedef struct {
	fcgi_extension **exts;

	size_t used;
	size_t size;
} fcgi_exts;


typedef struct {
	fcgi_exts *exts;

	array *ext_mapping;

	int debug;
} plugin_config;

typedef struct {
	size_t *ptr;
	size_t used;
	size_t size;
} buffer_uint;

typedef struct {
	char **ptr;

	size_t size;
	size_t used;
} char_array;

/* generic plugin data, shared between all connections */
typedef struct {
	PLUGIN_DATA;
	buffer_uint fcgi_request_id;

	buffer *fcgi_env;

	buffer *path;

	buffer *statuskey;

	http_resp *resp;

	plugin_config **config_storage;

	plugin_config conf; /* this is only used as long as no handler_ctx is setup */
} plugin_data;

/* connection specific data */
typedef enum {
	FCGI_STATE_UNSET,
	FCGI_STATE_INIT,
	FCGI_STATE_CONNECT_DELAYED,
	FCGI_STATE_PREPARE_WRITE,
	FCGI_STATE_WRITE,
	FCGI_STATE_READ
} fcgi_connection_state_t;

typedef struct {
	fcgi_proc *proc;
	fcgi_extension_host *host;
	fcgi_extension *ext;

	fcgi_connection_state_t state;
	time_t   state_timestamp;

	int      reconnects; /* number of reconnect attempts */

	chunkqueue *rb; /* the raw fcgi read-queue */
	chunkqueue *http_rb; /* the decoded read-queue for http-parsing */
	chunkqueue *wb; /* write queue */

	size_t    request_id;
	iosocket *sock;

	pid_t     pid;
	int       got_proc;

	int       send_content_body;

	plugin_config conf;

	connection *remote_conn;  /* dumb pointer */
	plugin_data *plugin_data; /* dumb pointer */
} handler_ctx;


/* ok, we need a prototype */
static handler_t fcgi_handle_fdevent(void *s, void *ctx, int revents);

int fastcgi_status_copy_procname(buffer *b, fcgi_extension_host *host, fcgi_proc *proc) {
	buffer_copy_string(b, "fastcgi.backend.");
	buffer_append_string_buffer(b, host->id);
	if (proc) {
		buffer_append_string(b, ".");
		buffer_append_long(b, proc->id);
	}

	return 0;
}

int fastcgi_status_init(server *srv, buffer *b, fcgi_extension_host *host, fcgi_proc *proc) {
#define CLEAN(x) \
	fastcgi_status_copy_procname(b, host, proc); \
	buffer_append_string(b, x); \
	status_counter_set(CONST_BUF_LEN(b), 0);

	CLEAN(".disabled");
	CLEAN(".died");
	CLEAN(".overloaded");
	CLEAN(".connected");
	CLEAN(".load");

#undef CLEAN

#define CLEAN(x) \
	fastcgi_status_copy_procname(b, host, NULL); \
	buffer_append_string(b, x); \
	status_counter_set(CONST_BUF_LEN(b), 0);

	CLEAN(".load");

#undef CLEAN

	return 0;
}

static handler_ctx * handler_ctx_init() {
	handler_ctx * hctx;

	hctx = calloc(1, sizeof(*hctx));
	assert(hctx);

	hctx->request_id = 0;
	hctx->state = FCGI_STATE_INIT;
	hctx->proc = NULL;

	hctx->sock = iosocket_init();

	hctx->reconnects = 0;
	hctx->send_content_body = 1;

	hctx->rb = chunkqueue_init();
	hctx->http_rb = chunkqueue_init();
	hctx->wb = chunkqueue_init();

	return hctx;
}

static void handler_ctx_free(handler_ctx *hctx) {
	if (hctx->host) {
		hctx->host->load--;
		hctx->host = NULL;
	}

	chunkqueue_free(hctx->rb);
	chunkqueue_free(hctx->http_rb);
	chunkqueue_free(hctx->wb);

	iosocket_free(hctx->sock);

	free(hctx);
}

fcgi_proc *fastcgi_process_init() {
	fcgi_proc *f;

	f = calloc(1, sizeof(*f));
	f->unixsocket = buffer_init();
	f->connection_name = buffer_init();

	f->prev = NULL;
	f->next = NULL;

	return f;
}

void fastcgi_process_free(fcgi_proc *f) {
	if (!f) return;

	fastcgi_process_free(f->next);

	buffer_free(f->unixsocket);
	buffer_free(f->connection_name);

	free(f);
}

fcgi_extension_host *fastcgi_host_init() {
	fcgi_extension_host *f;

	f = calloc(1, sizeof(*f));

	f->id = buffer_init();
	f->host = buffer_init();
	f->unixsocket = buffer_init();
	f->docroot = buffer_init();
	f->bin_path = buffer_init();
	f->bin_env = array_init();
	f->bin_env_copy = array_init();
	f->strip_request_uri = buffer_init();

	return f;
}

void fastcgi_host_free(fcgi_extension_host *h) {
	if (!h) return;

	buffer_free(h->id);
	buffer_free(h->host);
	buffer_free(h->unixsocket);
	buffer_free(h->docroot);
	buffer_free(h->bin_path);
	buffer_free(h->strip_request_uri);
	array_free(h->bin_env);
	array_free(h->bin_env_copy);

	fastcgi_process_free(h->first);
	fastcgi_process_free(h->unused_procs);

	free(h);

}

fcgi_exts *fastcgi_extensions_init() {
	fcgi_exts *f;

	f = calloc(1, sizeof(*f));

	return f;
}

void fastcgi_extensions_free(fcgi_exts *f) {
	size_t i;

	if (!f) return;

	for (i = 0; i < f->used; i++) {
		fcgi_extension *fe;
		size_t j;

		fe = f->exts[i];

		for (j = 0; j < fe->used; j++) {
			fcgi_extension_host *h;

			h = fe->hosts[j];

			fastcgi_host_free(h);
		}

		buffer_free(fe->key);
		free(fe->hosts);

		free(fe);
	}

	free(f->exts);

	free(f);
}

int fastcgi_extension_insert(fcgi_exts *ext, buffer *key, fcgi_extension_host *fh) {
	fcgi_extension *fe;
	size_t i;

	/* there is something */

	for (i = 0; i < ext->used; i++) {
		if (buffer_is_equal(key, ext->exts[i]->key)) {
			break;
		}
	}

	if (i == ext->used) {
		/* filextension is new */
		fe = calloc(1, sizeof(*fe));
		assert(fe);
		fe->key = buffer_init();
		buffer_copy_string_buffer(fe->key, key);

		/* */

		if (ext->size == 0) {
			ext->size = 8;
			ext->exts = malloc(ext->size * sizeof(*(ext->exts)));
			assert(ext->exts);
		} else if (ext->used == ext->size) {
			ext->size += 8;
			ext->exts = realloc(ext->exts, ext->size * sizeof(*(ext->exts)));
			assert(ext->exts);
		}
		ext->exts[ext->used++] = fe;
	} else {
		fe = ext->exts[i];
	}

	if (fe->size == 0) {
		fe->size = 4;
		fe->hosts = malloc(fe->size * sizeof(*(fe->hosts)));
		assert(fe->hosts);
	} else if (fe->size == fe->used) {
		fe->size += 4;
		fe->hosts = realloc(fe->hosts, fe->size * sizeof(*(fe->hosts)));
		assert(fe->hosts);
	}

	fe->hosts[fe->used++] = fh;

	return 0;

}

INIT_FUNC(mod_fastcgi_init) {
	plugin_data *p;

	p = calloc(1, sizeof(*p));

	p->fcgi_env = buffer_init();

	p->path = buffer_init();

	p->resp = http_response_init();

	p->statuskey = buffer_init();

	return p;
}


FREE_FUNC(mod_fastcgi_free) {
	plugin_data *p = p_d;
	buffer_uint *r = &(p->fcgi_request_id);

	UNUSED(srv);

	if (r->ptr) free(r->ptr);

	buffer_free(p->fcgi_env);
	buffer_free(p->path);
	buffer_free(p->statuskey);

	http_response_free(p->resp);

	if (p->config_storage) {
		size_t i, j, n;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];
			fcgi_exts *exts;

			if (!s) continue;

			exts = s->exts;

			for (j = 0; j < exts->used; j++) {
				fcgi_extension *ex;

				ex = exts->exts[j];

				for (n = 0; n < ex->used; n++) {
					fcgi_proc *proc;
					fcgi_extension_host *host;

					host = ex->hosts[n];

					for (proc = host->first; proc; proc = proc->next) {
						if (proc->pid != 0) kill(proc->pid, SIGTERM);

						if (proc->is_local &&
						    !buffer_is_empty(proc->unixsocket)) {
							unlink(proc->unixsocket->ptr);
						}
					}

					for (proc = host->unused_procs; proc; proc = proc->next) {
						if (proc->pid != 0) kill(proc->pid, SIGTERM);

						if (proc->is_local &&
						    !buffer_is_empty(proc->unixsocket)) {
							unlink(proc->unixsocket->ptr);
						}
					}
				}
			}

			fastcgi_extensions_free(s->exts);
			array_free(s->ext_mapping);

			free(s);
		}
		free(p->config_storage);
	}

	free(p);

	return HANDLER_GO_ON;
}

static int env_add(char_array *env, const char *key, size_t key_len, const char *val, size_t val_len) {
	char *dst;

	if (!key || !val) return -1;

	dst = malloc(key_len + val_len + 3);
	memcpy(dst, key, key_len);
	dst[key_len] = '=';
	/* add the \0 from the value */
	memcpy(dst + key_len + 1, val, val_len + 1);

	if (env->size == 0) {
		env->size = 16;
		env->ptr = malloc(env->size * sizeof(*env->ptr));
	} else if (env->size == env->used + 1) {
		env->size += 16;
		env->ptr = realloc(env->ptr, env->size * sizeof(*env->ptr));
	}

	env->ptr[env->used++] = dst;

	return 0;
}

static int parse_binpath(char_array *env, buffer *b) {
	char *start;
	size_t i;
	/* search for spaces */

	start = b->ptr;
	for (i = 0; i < b->used - 1; i++) {
		switch(b->ptr[i]) {
		case ' ':
		case '\t':
			/* a WS, stop here and copy the argument */

			if (env->size == 0) {
				env->size = 16;
				env->ptr = malloc(env->size * sizeof(*env->ptr));
			} else if (env->size == env->used) {
				env->size += 16;
				env->ptr = realloc(env->ptr, env->size * sizeof(*env->ptr));
			}

			b->ptr[i] = '\0';

			env->ptr[env->used++] = start;

			start = b->ptr + i + 1;
			break;
		default:
			break;
		}
	}

	if (env->size == 0) {
		env->size = 16;
		env->ptr = malloc(env->size * sizeof(*env->ptr));
	} else if (env->size == env->used) { /* we need one extra for the terminating NULL */
		env->size += 16;
		env->ptr = realloc(env->ptr, env->size * sizeof(*env->ptr));
	}

	/* the rest */
	env->ptr[env->used++] = start;

	if (env->size == 0) {
		env->size = 16;
		env->ptr = malloc(env->size * sizeof(*env->ptr));
	} else if (env->size == env->used) { /* we need one extra for the terminating NULL */
		env->size += 16;
		env->ptr = realloc(env->ptr, env->size * sizeof(*env->ptr));
	}

	/* terminate */
	env->ptr[env->used++] = NULL;

	return 0;
}

static int fcgi_spawn_connection(server *srv,
				 plugin_data *p,
				 fcgi_extension_host *host,
				 fcgi_proc *proc) {
	int fcgi_fd;
	int socket_type, status;
	struct timeval tv = { 0, 100 * 1000 };
#ifdef HAVE_SYS_UN_H
	struct sockaddr_un fcgi_addr_un;
#endif
	struct sockaddr_in fcgi_addr_in;
	struct sockaddr *fcgi_addr;

	socklen_t servlen;

#ifndef HAVE_FORK
	return -1;
#endif

	if (p->conf.debug) {
		log_error_write(srv, __FILE__, __LINE__, "sdb",
				"new proc, socket:", proc->port, proc->unixsocket);
	}

	if (!buffer_is_empty(proc->unixsocket)) {
		memset(&fcgi_addr, 0, sizeof(fcgi_addr));

#ifdef HAVE_SYS_UN_H
		fcgi_addr_un.sun_family = AF_UNIX;
		strcpy(fcgi_addr_un.sun_path, proc->unixsocket->ptr);

		servlen = SUN_LEN(&fcgi_addr_un);

		socket_type = AF_UNIX;
		fcgi_addr = (struct sockaddr *) &fcgi_addr_un;

		buffer_copy_string(proc->connection_name, "unix:");
		buffer_append_string_buffer(proc->connection_name, proc->unixsocket);

#else
		log_error_write(srv, __FILE__, __LINE__, "s",
				"ERROR: Unix Domain sockets are not supported.");
		return -1;
#endif
	} else {
		fcgi_addr_in.sin_family = AF_INET;

		if (buffer_is_empty(host->host)) {
			fcgi_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);
		} else {
			struct hostent *he;

			/* set a usefull default */
			fcgi_addr_in.sin_addr.s_addr = htonl(INADDR_ANY);


			if (NULL == (he = gethostbyname(host->host->ptr))) {
				log_error_write(srv, __FILE__, __LINE__,
						"sdb", "gethostbyname failed: ",
						h_errno, host->host);
				return -1;
			}

			if (he->h_addrtype != AF_INET) {
				log_error_write(srv, __FILE__, __LINE__, "sd", "addr-type != AF_INET: ", he->h_addrtype);
				return -1;
			}

			if (he->h_length != sizeof(struct in_addr)) {
				log_error_write(srv, __FILE__, __LINE__, "sd", "addr-length != sizeof(in_addr): ", he->h_length);
				return -1;
			}

			memcpy(&(fcgi_addr_in.sin_addr.s_addr), he->h_addr_list[0], he->h_length);

		}
		fcgi_addr_in.sin_port = htons(proc->port);
		servlen = sizeof(fcgi_addr_in);

		socket_type = AF_INET;
		fcgi_addr = (struct sockaddr *) &fcgi_addr_in;

		buffer_copy_string(proc->connection_name, "tcp:");
		buffer_append_string_buffer(proc->connection_name, host->host);
		buffer_append_string(proc->connection_name, ":");
		buffer_append_long(proc->connection_name, proc->port);
	}

	if (-1 == (fcgi_fd = socket(socket_type, SOCK_STREAM, 0))) {
		log_error_write(srv, __FILE__, __LINE__, "ss",
				"failed:", strerror(errno));
		return -1;
	}

	if (-1 == connect(fcgi_fd, fcgi_addr, servlen)) {
		/* server is not up, spawn in  */
		pid_t child;
		int val;

		if (errno != ENOENT &&
		    !buffer_is_empty(proc->unixsocket)) {
			unlink(proc->unixsocket->ptr);
		}

		close(fcgi_fd);

		/* reopen socket */
		if (-1 == (fcgi_fd = socket(socket_type, SOCK_STREAM, 0))) {
			log_error_write(srv, __FILE__, __LINE__, "ss",
				"socket failed:", strerror(errno));
			return -1;
		}

		val = 1;
		if (setsockopt(fcgi_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0) {
			log_error_write(srv, __FILE__, __LINE__, "ss",
					"socketsockopt failed:", strerror(errno));
			return -1;
		}

		/* create socket */
		if (-1 == bind(fcgi_fd, fcgi_addr, servlen)) {
			log_error_write(srv, __FILE__, __LINE__, "sbs",
				"bind failed for:",
				proc->connection_name,
				strerror(errno));
			return -1;
		}

		if (-1 == listen(fcgi_fd, 1024)) {
			log_error_write(srv, __FILE__, __LINE__, "ss",
				"listen failed:", strerror(errno));
			return -1;
		}

#ifndef _WIN32
		switch ((child = fork())) {
		case 0: {
			size_t i = 0;
			char *c;
			char_array env;
			char_array arg;

			/* create environment */
			env.ptr = NULL;
			env.size = 0;
			env.used = 0;

			arg.ptr = NULL;
			arg.size = 0;
			arg.used = 0;

			if(fcgi_fd != FCGI_LISTENSOCK_FILENO) {
				close(FCGI_LISTENSOCK_FILENO);
				dup2(fcgi_fd, FCGI_LISTENSOCK_FILENO);
				close(fcgi_fd);
			}

			/* we don't need the client socket */
			for (i = 3; i < 256; i++) {
				close(i);
			}

			/* build clean environment */
			if (host->bin_env_copy->used) {
				for (i = 0; i < host->bin_env_copy->used; i++) {
					data_string *ds = (data_string *)host->bin_env_copy->data[i];
					char *ge;

					if (NULL != (ge = getenv(ds->value->ptr))) {
						env_add(&env, CONST_BUF_LEN(ds->value), ge, strlen(ge));
					}
				}
			} else {
				for (i = 0; environ[i]; i++) {
					char *eq;

					if (NULL != (eq = strchr(environ[i], '='))) {
						env_add(&env, environ[i], eq - environ[i], eq+1, strlen(eq+1));
					}
				}
			}

			/* create environment */
			for (i = 0; i < host->bin_env->used; i++) {
				data_string *ds = (data_string *)host->bin_env->data[i];

				env_add(&env, CONST_BUF_LEN(ds->key), CONST_BUF_LEN(ds->value));
			}

			for (i = 0; i < env.used; i++) {
				/* search for PHP_FCGI_CHILDREN */
				if (0 == strncmp(env.ptr[i], "PHP_FCGI_CHILDREN=", sizeof("PHP_FCGI_CHILDREN=") - 1)) break;
			}

			/* not found, add a default */
			if (i == env.used) {
				env_add(&env, CONST_STR_LEN("PHP_FCGI_CHILDREN"), CONST_STR_LEN("1"));
			}

			env.ptr[env.used] = NULL;

			parse_binpath(&arg, host->bin_path);

			/* chdir into the base of the bin-path,
			 * search for the last / */
			if (NULL != (c = strrchr(arg.ptr[0], '/'))) {
				*c = '\0';

				/* change to the physical directory */
				if (-1 == chdir(arg.ptr[0])) {
					*c = '/';
					log_error_write(srv, __FILE__, __LINE__, "sss", "chdir failed:", strerror(errno), arg.ptr[0]);
				}
				*c = '/';
			}


			/* exec the cgi */
			execve(arg.ptr[0], arg.ptr, env.ptr);

			log_error_write(srv, __FILE__, __LINE__, "sbs",
					"execve failed for:", host->bin_path, strerror(errno));

			exit(errno);

			break;
		}
		case -1:
			/* error */
			break;
		default:
			/* father */

			/* wait */
			select(0, NULL, NULL, NULL, &tv);

			switch (waitpid(child, &status, WNOHANG)) {
			case 0:
				/* child still running after timeout, good */
				break;
			case -1:
				/* no PID found ? should never happen */
				log_error_write(srv, __FILE__, __LINE__, "ss",
						"pid not found:", strerror(errno));
				return -1;
			default:
				log_error_write(srv, __FILE__, __LINE__, "sbs",
						"the fastcgi-backend", host->bin_path, "failed to start:");
				/* the child should not terminate at all */
				if (WIFEXITED(status)) {
					log_error_write(srv, __FILE__, __LINE__, "sdb",
							"child exited with status",
							WEXITSTATUS(status), host->bin_path);
					log_error_write(srv, __FILE__, __LINE__, "s",
							"if you try do run PHP as FastCGI backend make sure you use the FastCGI enabled version.\n"
							"You can find out if it is the right one by executing 'php -v' and it should display '(cgi-fcgi)' "
							"in the output, NOT (cgi) NOR (cli)\n"
							"For more information check http://www.lighttpd.net/documentation/fastcgi.html#preparing-php-as-a-fastcgi-program");
					log_error_write(srv, __FILE__, __LINE__, "s",
							"If this is PHP on Gentoo add fastcgi to the USE flags");
				} else if (WIFSIGNALED(status)) {
					log_error_write(srv, __FILE__, __LINE__, "sd",
							"terminated by signal:",
							WTERMSIG(status));

					if (WTERMSIG(status) == 11) {
						log_error_write(srv, __FILE__, __LINE__, "s",
								"to be exact: it seg-fault, crashed, died, ... you get the idea." );
						log_error_write(srv, __FILE__, __LINE__, "s",
								"If this is PHP try to remove the byte-code caches for now and try again.");
					}
				} else {
					log_error_write(srv, __FILE__, __LINE__, "sd",
							"child died somehow:",
							status);
				}
				return -1;
			}

			/* register process */
			proc->pid = child;
			proc->last_used = srv->cur_ts;
			proc->is_local = 1;

			break;
		}
#endif
	} else {
		proc->is_local = 0;
		proc->pid = 0;

		if (p->conf.debug) {
			log_error_write(srv, __FILE__, __LINE__, "sb",
					"(debug) socket is already used, won't spawn:",
					proc->connection_name);
		}
	}

	proc->state = PROC_STATE_RUNNING;
	host->active_procs++;

	close(fcgi_fd);

	return 0;
}


SETDEFAULTS_FUNC(mod_fastcgi_set_defaults) {
	plugin_data *p = p_d;
	data_unset *du;
	size_t i = 0;
	buffer *fcgi_mode = buffer_init();

	config_values_t cv[] = {
		{ "fastcgi.server",              NULL, T_CONFIG_LOCAL, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
		{ "fastcgi.debug",               NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },       /* 1 */
		{ "fastcgi.map-extensions",      NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },       /* 2 */
		{ NULL,                          NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};

	p->config_storage = calloc(1, srv->config_context->used * sizeof(specific_config *));

	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;
		array *ca;

		s = malloc(sizeof(plugin_config));
		s->exts          = fastcgi_extensions_init();
		s->debug         = 0;
		s->ext_mapping   = array_init();

		cv[0].destination = s->exts;
		cv[1].destination = &(s->debug);
		cv[2].destination = s->ext_mapping;

		p->config_storage[i] = s;
		ca = ((data_config *)srv->config_context->data[i])->value;

		if (0 != config_insert_values_global(srv, ca, cv)) {
			return HANDLER_ERROR;
		}

		/*
		 * <key> = ( ... )
		 */

		if (NULL != (du = array_get_element(ca, "fastcgi.server"))) {
			size_t j;
			data_array *da = (data_array *)du;

			if (du->type != TYPE_ARRAY) {
				log_error_write(srv, __FILE__, __LINE__, "sss",
						"unexpected type for key: ", "fastcgi.server", "array of strings");

				return HANDLER_ERROR;
			}


			/*
			 * fastcgi.server = ( "<ext>" => ( ... ),
			 *                    "<ext>" => ( ... ) )
			 */

			for (j = 0; j < da->value->used; j++) {
				size_t n;
				data_array *da_ext = (data_array *)da->value->data[j];

				if (da->value->data[j]->type != TYPE_ARRAY) {
					log_error_write(srv, __FILE__, __LINE__, "sssbs",
							"unexpected type for key: ", "fastcgi.server",
							"[", da->value->data[j]->key, "](string)");

					return HANDLER_ERROR;
				}

				/*
				 * da_ext->key == name of the extension
				 */

				/*
				 * fastcgi.server = ( "<ext>" =>
				 *                     ( "<host>" => ( ... ),
				 *                       "<host>" => ( ... )
				 *                     ),
				 *                    "<ext>" => ... )
				 */

				for (n = 0; n < da_ext->value->used; n++) {
					data_array *da_host = (data_array *)da_ext->value->data[n];

					fcgi_extension_host *host;

					config_values_t fcv[] = {
						{ "host",              NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 0 */
						{ "docroot",           NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 1 */
						{ "mode",              NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 2 */
						{ "socket",            NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 3 */
						{ "bin-path",          NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },       /* 4 */

						{ "check-local",       NULL, T_CONFIG_BOOLEAN, T_CONFIG_SCOPE_CONNECTION },      /* 5 */
						{ "port",              NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },        /* 6 */
						{ "min-procs-not-working",         NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },        /* 7 this is broken for now */
						{ "max-procs",         NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },        /* 8 */
						{ "max-load-per-proc", NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },        /* 9 */
						{ "idle-timeout",      NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },        /* 10 */
						{ "disable-time",      NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },        /* 11 */

						{ "bin-environment",   NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },        /* 12 */
						{ "bin-copy-environment", NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },     /* 13 */

						{ "broken-scriptfilename", NULL, T_CONFIG_BOOLEAN, T_CONFIG_SCOPE_CONNECTION },  /* 14 */
						{ "allow-x-send-file", NULL, T_CONFIG_BOOLEAN, T_CONFIG_SCOPE_CONNECTION },      /* 15 */
						{ "strip-request-uri",  NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },      /* 16 */

						{ NULL,                NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
					};

					if (da_host->type != TYPE_ARRAY) {
						log_error_write(srv, __FILE__, __LINE__, "ssSBS",
								"unexpected type for key:",
								"fastcgi.server",
								"[", da_host->key, "](string)");

						return HANDLER_ERROR;
					}

					host = fastcgi_host_init();

					buffer_copy_string_buffer(host->id, da_host->key);

					host->check_local  = 1;
					host->min_procs    = 4;
					host->max_procs    = 4;
					host->max_load_per_proc = 1;
					host->idle_timeout = 60;
					host->mode = FCGI_RESPONDER;
					host->disable_time = 60;
					host->break_scriptfilename_for_php = 0;
					host->allow_xsendfile = 0; /* handle X-LIGHTTPD-send-file */

					fcv[0].destination = host->host;
					fcv[1].destination = host->docroot;
					fcv[2].destination = fcgi_mode;
					fcv[3].destination = host->unixsocket;
					fcv[4].destination = host->bin_path;

					fcv[5].destination = &(host->check_local);
					fcv[6].destination = &(host->port);
					fcv[7].destination = &(host->min_procs);
					fcv[8].destination = &(host->max_procs);
					fcv[9].destination = &(host->max_load_per_proc);
					fcv[10].destination = &(host->idle_timeout);
					fcv[11].destination = &(host->disable_time);

					fcv[12].destination = host->bin_env;
					fcv[13].destination = host->bin_env_copy;
					fcv[14].destination = &(host->break_scriptfilename_for_php);
					fcv[15].destination = &(host->allow_xsendfile);
					fcv[16].destination = host->strip_request_uri;

					if (0 != config_insert_values_internal(srv, da_host->value, fcv)) {
						return HANDLER_ERROR;
					}

					if ((!buffer_is_empty(host->host) || host->port) &&
					    !buffer_is_empty(host->unixsocket)) {
						log_error_write(srv, __FILE__, __LINE__, "sbsbsbs",
								"either host/port or socket have to be set in:",
								da->key, "= (",
								da_ext->key, " => (",
								da_host->key, " ( ...");

						return HANDLER_ERROR;
					}

					if (!buffer_is_empty(host->unixsocket)) {
						/* unix domain socket */

						if (host->unixsocket->used > UNIX_PATH_MAX - 2) {
							log_error_write(srv, __FILE__, __LINE__, "sbsbsbs",
									"unixsocket is too long in:",
									da->key, "= (",
									da_ext->key, " => (",
									da_host->key, " ( ...");

							return HANDLER_ERROR;
						}
					} else {
						/* tcp/ip */

						if (buffer_is_empty(host->host) &&
						    buffer_is_empty(host->bin_path)) {
							log_error_write(srv, __FILE__, __LINE__, "sbsbsbs",
									"host or binpath have to be set in:",
									da->key, "= (",
									da_ext->key, " => (",
									da_host->key, " ( ...");

							return HANDLER_ERROR;
						} else if (host->port == 0) {
							log_error_write(srv, __FILE__, __LINE__, "sbsbsbs",
									"port has to be set in:",
									da->key, "= (",
									da_ext->key, " => (",
									da_host->key, " ( ...");

							return HANDLER_ERROR;
						}
					}

					if (!buffer_is_empty(host->bin_path)) {
						/* a local socket + self spawning */
						size_t pno;

						/* HACK:  just to make sure the adaptive spawing is disabled */
						host->min_procs = host->max_procs;

						if (host->min_procs > host->max_procs) host->max_procs = host->min_procs;
						if (host->max_load_per_proc < 1) host->max_load_per_proc = 0;

						if (s->debug) {
							log_error_write(srv, __FILE__, __LINE__, "ssbsdsbsdsd",
									"--- fastcgi spawning local",
									"\n\tproc:", host->bin_path,
									"\n\tport:", host->port,
									"\n\tsocket", host->unixsocket,
									"\n\tmin-procs:", host->min_procs,
									"\n\tmax-procs:", host->max_procs);
						}

						for (pno = 0; pno < host->min_procs; pno++) {
							fcgi_proc *proc;

							proc = fastcgi_process_init();
							proc->id = host->num_procs++;
							host->max_id++;

							if (buffer_is_empty(host->unixsocket)) {
								proc->port = host->port + pno;
							} else {
								buffer_copy_string_buffer(proc->unixsocket, host->unixsocket);
								buffer_append_string(proc->unixsocket, "-");
								buffer_append_long(proc->unixsocket, pno);
							}

							if (s->debug) {
								log_error_write(srv, __FILE__, __LINE__, "ssdsbsdsd",
										"--- fastcgi spawning",
										"\n\tport:", host->port,
										"\n\tsocket", host->unixsocket,
										"\n\tcurrent:", pno, "/", host->min_procs);
							}

							if (fcgi_spawn_connection(srv, p, host, proc)) {
								log_error_write(srv, __FILE__, __LINE__, "s",
										"[ERROR]: spawning fcgi failed.");
								return HANDLER_ERROR;
							}

							fastcgi_status_init(srv, p->statuskey, host, proc);

							proc->next = host->first;
							if (host->first) 	host->first->prev = proc;

							host->first = proc;
						}
					} else {
						fcgi_proc *proc;

						proc = fastcgi_process_init();
						proc->id = host->num_procs++;
						host->max_id++;
						host->active_procs++;
						proc->state = PROC_STATE_RUNNING;

						if (buffer_is_empty(host->unixsocket)) {
							proc->port = host->port;
						} else {
							buffer_copy_string_buffer(proc->unixsocket, host->unixsocket);
						}

						fastcgi_status_init(srv, p->statuskey, host, proc);

						host->first = proc;

						host->min_procs = 1;
						host->max_procs = 1;
					}

					if (!buffer_is_empty(fcgi_mode)) {
						if (strcmp(fcgi_mode->ptr, "responder") == 0) {
							host->mode = FCGI_RESPONDER;
						} else if (strcmp(fcgi_mode->ptr, "authorizer") == 0) {
							host->mode = FCGI_AUTHORIZER;
							if (buffer_is_empty(host->docroot)) {
								log_error_write(srv, __FILE__, __LINE__, "s",
										"ERROR: docroot is required for authorizer mode.");
								return HANDLER_ERROR;
							}
						} else {
							log_error_write(srv, __FILE__, __LINE__, "sbs",
									"WARNING: unknown fastcgi mode:",
									fcgi_mode, "(ignored, mode set to responder)");
						}
					}

					/* if extension already exists, take it */
					fastcgi_extension_insert(s->exts, da_ext->key, host);
				}
			}
		}
	}

	buffer_free(fcgi_mode);

	return HANDLER_GO_ON;
}

static int fcgi_set_state(server *srv, handler_ctx *hctx, fcgi_connection_state_t state) {
	hctx->state = state;
	hctx->state_timestamp = srv->cur_ts;

	return 0;
}


static size_t fcgi_requestid_new(server *srv, plugin_data *p) {
	size_t m = 0;
	size_t i;
	buffer_uint *r = &(p->fcgi_request_id);

	UNUSED(srv);

	for (i = 0; i < r->used; i++) {
		if (r->ptr[i] > m) m = r->ptr[i];
	}

	if (r->size == 0) {
		r->size = 16;
		r->ptr = malloc(sizeof(*r->ptr) * r->size);
	} else if (r->used == r->size) {
		r->size += 16;
		r->ptr = realloc(r->ptr, sizeof(*r->ptr) * r->size);
	}

	r->ptr[r->used++] = ++m;

	return m;
}

static int fcgi_requestid_del(server *srv, plugin_data *p, size_t request_id) {
	size_t i;
	buffer_uint *r = &(p->fcgi_request_id);

	UNUSED(srv);

	for (i = 0; i < r->used; i++) {
		if (r->ptr[i] == request_id) break;
	}

	if (i != r->used) {
		/* found */

		if (i != r->used - 1) {
			r->ptr[i] = r->ptr[r->used - 1];
		}
		r->used--;
	}

	return 0;
}
void fcgi_connection_close(server *srv, handler_ctx *hctx) {
	plugin_data *p;
	connection  *con;

	if (NULL == hctx) return;

	p    = hctx->plugin_data;
	con  = hctx->remote_conn;

	if (con->mode != p->id) {
		return;
	}

	if (hctx->sock->fd != -1) {
		fdevent_event_del(srv->ev, hctx->sock);
		fdevent_unregister(srv->ev, hctx->sock);
		closesocket(hctx->sock->fd);
		hctx->sock->fd = -1;

		srv->cur_fds--;
	}

	if (hctx->request_id != 0) {
		fcgi_requestid_del(srv, p, hctx->request_id);
	}

	if (hctx->host && hctx->proc) {
		if (hctx->got_proc) {
			/* after the connect the process gets a load */
			hctx->proc->load--;

			status_counter_dec(CONST_STR_LEN("fastcgi.active-requests"));

			fastcgi_status_copy_procname(p->statuskey, hctx->host, hctx->proc);
			buffer_append_string(p->statuskey, ".load");

			status_counter_set(CONST_BUF_LEN(p->statuskey), hctx->proc->load);

			if (p->conf.debug) {
				log_error_write(srv, __FILE__, __LINE__, "ssdsbsd",
						"released proc:",
						"pid:", hctx->proc->pid,
						"socket:", hctx->proc->connection_name,
						"load:", hctx->proc->load);
			}
		}
	}


	handler_ctx_free(hctx);
	con->plugin_ctx[p->id] = NULL;
}

static int fcgi_reconnect(server *srv, handler_ctx *hctx) {
	plugin_data *p    = hctx->plugin_data;

	/* child died
	 *
	 * 1.
	 *
	 * connect was ok, connection was accepted
	 * but the php accept loop checks after the accept if it should die or not.
	 *
	 * if yes we can only detect it at a write()
	 *
	 * next step is resetting this attemp and setup a connection again
	 *
	 * if we have more then 5 reconnects for the same request, die
	 *
	 * 2.
	 *
	 * we have a connection but the child died by some other reason
	 *
	 */

	if (hctx->sock->fd != -1) {
		fdevent_event_del(srv->ev, hctx->sock);
		fdevent_unregister(srv->ev, hctx->sock);
		close(hctx->sock->fd);
		srv->cur_fds--;
		hctx->sock->fd = -1;
	}

	fcgi_requestid_del(srv, p, hctx->request_id);

	fcgi_set_state(srv, hctx, FCGI_STATE_INIT);

	hctx->request_id = 0;
	hctx->reconnects++;

	if (p->conf.debug > 2) {
		if (hctx->proc) {
			log_error_write(srv, __FILE__, __LINE__, "sdb",
					"release proc for reconnect:",
					hctx->proc->pid, hctx->proc->connection_name);
		} else {
			log_error_write(srv, __FILE__, __LINE__, "sb",
					"release proc for reconnect:",
					hctx->host->unixsocket);
		}
	}

	if (hctx->proc && hctx->got_proc) {
		hctx->proc->load--;
	}

	/* perhaps another host gives us more luck */
	hctx->host->load--;
	hctx->host = NULL;

	return 0;
}


static handler_t fcgi_connection_reset(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;

	fcgi_connection_close(srv, con->plugin_ctx[p->id]);

	return HANDLER_GO_ON;
}


static int fcgi_env_add(buffer *env, const char *key, size_t key_len, const char *val, size_t val_len) {
	size_t len;

	if (!key || !val) return -1;

	len = key_len + val_len;

	len += key_len > 127 ? 4 : 1;
	len += val_len > 127 ? 4 : 1;

	buffer_prepare_append(env, len);

	if (key_len > 127) {
		env->ptr[env->used++] = ((key_len >> 24) & 0xff) | 0x80;
		env->ptr[env->used++] = (key_len >> 16) & 0xff;
		env->ptr[env->used++] = (key_len >> 8) & 0xff;
		env->ptr[env->used++] = (key_len >> 0) & 0xff;
	} else {
		env->ptr[env->used++] = (key_len >> 0) & 0xff;
	}

	if (val_len > 127) {
		env->ptr[env->used++] = ((val_len >> 24) & 0xff) | 0x80;
		env->ptr[env->used++] = (val_len >> 16) & 0xff;
		env->ptr[env->used++] = (val_len >> 8) & 0xff;
		env->ptr[env->used++] = (val_len >> 0) & 0xff;
	} else {
		env->ptr[env->used++] = (val_len >> 0) & 0xff;
	}

	memcpy(env->ptr + env->used, key, key_len);
	env->used += key_len;
	memcpy(env->ptr + env->used, val, val_len);
	env->used += val_len;

	return 0;
}

static int fcgi_header(FCGI_Header * header, unsigned char type, size_t request_id, int contentLength, unsigned char paddingLength) {
	header->version = FCGI_VERSION_1;
	header->type = type;
	header->requestIdB0 = request_id & 0xff;
	header->requestIdB1 = (request_id >> 8) & 0xff;
	header->contentLengthB0 = contentLength & 0xff;
	header->contentLengthB1 = (contentLength >> 8) & 0xff;
	header->paddingLength = paddingLength;
	header->reserved = 0;

	return 0;
}
/**
 *
 * returns
 *   -1 error
 *    0 connected
 *    1 not connected yet
 */

typedef enum {
	CONNECTION_UNSET,
	CONNECTION_OK,
	CONNECTION_DELAYED, /* retry after event, take same host */
	CONNECTION_OVERLOADED, /* disable for 1 seconds, take another backend */
	CONNECTION_DEAD /* disable for 60 seconds, take another backend */
} connection_result_t;

static connection_result_t fcgi_establish_connection(server *srv, handler_ctx *hctx) {
	struct sockaddr *fcgi_addr;
	struct sockaddr_in fcgi_addr_in;
#ifdef HAVE_SYS_UN_H
	struct sockaddr_un fcgi_addr_un;
#endif
	socklen_t servlen;

	fcgi_extension_host *host = hctx->host;
	fcgi_proc *proc   = hctx->proc;
	int fcgi_fd       = hctx->sock->fd;

	memset(&fcgi_addr, 0, sizeof(fcgi_addr));

	if (!buffer_is_empty(proc->unixsocket)) {
#ifdef HAVE_SYS_UN_H
		/* use the unix domain socket */
		fcgi_addr_un.sun_family = AF_UNIX;
		strcpy(fcgi_addr_un.sun_path, proc->unixsocket->ptr);

		servlen = SUN_LEN(&fcgi_addr_un);

		fcgi_addr = (struct sockaddr *) &fcgi_addr_un;

		if (buffer_is_empty(proc->connection_name)) {
			/* on remote spawing we have to set the connection-name now */
			buffer_copy_string(proc->connection_name, "unix:");
			buffer_append_string_buffer(proc->connection_name, proc->unixsocket);
		}
#else
		return -1;
#endif
	} else {
		fcgi_addr_in.sin_family = AF_INET;

		if (0 == inet_aton(host->host->ptr, &(fcgi_addr_in.sin_addr))) {
			log_error_write(srv, __FILE__, __LINE__, "sbs",
					"converting IP-adress failed for", host->host,
					"\nBe sure to specify an IP address here");

			return -1;
		}

		fcgi_addr_in.sin_port = htons(proc->port);
		servlen = sizeof(fcgi_addr_in);

		fcgi_addr = (struct sockaddr *) &fcgi_addr_in;

		if (buffer_is_empty(proc->connection_name)) {
			/* on remote spawing we have to set the connection-name now */
			buffer_copy_string(proc->connection_name, "tcp:");
			buffer_append_string_buffer(proc->connection_name, host->host);
			buffer_append_string(proc->connection_name, ":");
			buffer_append_long(proc->connection_name, proc->port);
		}
	}

	if (-1 == connect(fcgi_fd, fcgi_addr, servlen)) {
		if (errno == EINPROGRESS ||
		    errno == EALREADY ||
		    errno == EINTR) {
			if (hctx->conf.debug > 2) {
				log_error_write(srv, __FILE__, __LINE__, "sb",
					"connect delayed, will continue later:", proc->connection_name);
			}

			return CONNECTION_DELAYED;
		} else if (errno == EAGAIN) {
			if (hctx->conf.debug) {
				log_error_write(srv, __FILE__, __LINE__, "sbsd",
					"This means that the you have more incoming requests than your fastcgi-backend can handle in parallel. "
					"Perhaps it helps to spawn more fastcgi backend or php-children, if not decrease server.max-connections."
					"The load for this fastcgi backend", proc->connection_name, "is", proc->load);
			}

			return CONNECTION_OVERLOADED;
		} else {
			log_error_write(srv, __FILE__, __LINE__, "sssb",
					"connect failed:",
					strerror(errno), "on",
					proc->connection_name);

			return CONNECTION_DEAD;
		}
	}

	hctx->reconnects = 0;
	if (hctx->conf.debug > 1) {
		log_error_write(srv, __FILE__, __LINE__, "sd",
				"connect succeeded: ", fcgi_fd);
	}

	return CONNECTION_OK;
}

static int fcgi_env_add_request_headers(server *srv, connection *con, plugin_data *p) {
	size_t i;

	for (i = 0; i < con->request.headers->used; i++) {
		data_string *ds;

		ds = (data_string *)con->request.headers->data[i];

		if (ds->value->used && ds->key->used) {
			size_t j;
			buffer_reset(srv->tmp_buf);

			if (0 != strcasecmp(ds->key->ptr, "CONTENT-TYPE")) {
				BUFFER_COPY_STRING_CONST(srv->tmp_buf, "HTTP_");
				srv->tmp_buf->used--;
			}

			buffer_prepare_append(srv->tmp_buf, ds->key->used + 2);
			for (j = 0; j < ds->key->used - 1; j++) {
				char c = '_';
				if (light_isalpha(ds->key->ptr[j])) {
					/* upper-case */
					c = ds->key->ptr[j] & ~32;
				} else if (light_isdigit(ds->key->ptr[j])) {
					/* copy */
					c = ds->key->ptr[j];
				}
				srv->tmp_buf->ptr[srv->tmp_buf->used++] = c;
			}
			srv->tmp_buf->ptr[srv->tmp_buf->used++] = '\0';

			fcgi_env_add(p->fcgi_env, CONST_BUF_LEN(srv->tmp_buf), CONST_BUF_LEN(ds->value));
		}
	}

	for (i = 0; i < con->environment->used; i++) {
		data_string *ds;

		ds = (data_string *)con->environment->data[i];

		if (ds->value->used && ds->key->used) {
			size_t j;
			buffer_reset(srv->tmp_buf);

			buffer_prepare_append(srv->tmp_buf, ds->key->used + 2);
			for (j = 0; j < ds->key->used - 1; j++) {
				char c = '_';
				if (light_isalpha(ds->key->ptr[j])) {
					/* upper-case */
					c = ds->key->ptr[j] & ~32;
				} else if (light_isdigit(ds->key->ptr[j])) {
					/* copy */
					c = ds->key->ptr[j];
				}
				srv->tmp_buf->ptr[srv->tmp_buf->used++] = c;
			}
			srv->tmp_buf->ptr[srv->tmp_buf->used++] = '\0';

			fcgi_env_add(p->fcgi_env, CONST_BUF_LEN(srv->tmp_buf), CONST_BUF_LEN(ds->value));
		}
	}

	return 0;
}


static int fcgi_create_env(server *srv, handler_ctx *hctx, size_t request_id) {
	FCGI_BeginRequestRecord beginRecord;
	FCGI_Header header;
	buffer *b;

	char buf[32];
	const char *s;
#ifdef HAVE_IPV6
	char b2[INET6_ADDRSTRLEN + 1];
#endif

	plugin_data *p    = hctx->plugin_data;
	fcgi_extension_host *host= hctx->host;

	connection *con   = hctx->remote_conn;
	server_socket *srv_sock = con->srv_socket;

	sock_addr our_addr;
	socklen_t our_addr_len;

	/* send FCGI_BEGIN_REQUEST */

	fcgi_header(&(beginRecord.header), FCGI_BEGIN_REQUEST, request_id, sizeof(beginRecord.body), 0);
	beginRecord.body.roleB0 = host->mode;
	beginRecord.body.roleB1 = 0;
	beginRecord.body.flags = 0;
	memset(beginRecord.body.reserved, 0, sizeof(beginRecord.body.reserved));

	b = chunkqueue_get_append_buffer(hctx->wb);

	buffer_copy_memory(b, (const char *)&beginRecord, sizeof(beginRecord));

	/* send FCGI_PARAMS */
	buffer_prepare_copy(p->fcgi_env, 1024);


	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SERVER_SOFTWARE"), CONST_STR_LEN(PACKAGE_NAME"/"PACKAGE_VERSION));

	if (con->server_name->used) {
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SERVER_NAME"), CONST_BUF_LEN(con->server_name));
	} else {
#ifdef HAVE_IPV6
		s = inet_ntop(srv_sock->addr.plain.sa_family,
			      srv_sock->addr.plain.sa_family == AF_INET6 ?
			      (const void *) &(srv_sock->addr.ipv6.sin6_addr) :
			      (const void *) &(srv_sock->addr.ipv4.sin_addr),
			      b2, sizeof(b2)-1);
#else
		s = inet_ntoa(srv_sock->addr.ipv4.sin_addr);
#endif
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SERVER_NAME"), s, strlen(s));
	}

	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("GATEWAY_INTERFACE"), CONST_STR_LEN("CGI/1.1"));

	ltostr(buf,
#ifdef HAVE_IPV6
	       ntohs(srv_sock->addr.plain.sa_family ? srv_sock->addr.ipv6.sin6_port : srv_sock->addr.ipv4.sin_port)
#else
	       ntohs(srv_sock->addr.ipv4.sin_port)
#endif
	       );

	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SERVER_PORT"), buf, strlen(buf));

	/* get the server-side of the connection to the client */
	our_addr_len = sizeof(our_addr);

	if (-1 == getsockname(con->sock->fd, &(our_addr.plain), &our_addr_len)) {
		s = inet_ntop_cache_get_ip(srv, &(srv_sock->addr));
	} else {
		s = inet_ntop_cache_get_ip(srv, &(our_addr));
	}
	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SERVER_ADDR"), s, strlen(s));

	ltostr(buf,
#ifdef HAVE_IPV6
	       ntohs(con->dst_addr.plain.sa_family ? con->dst_addr.ipv6.sin6_port : con->dst_addr.ipv4.sin_port)
#else
	       ntohs(con->dst_addr.ipv4.sin_port)
#endif
	       );

	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REMOTE_PORT"), buf, strlen(buf));

	s = inet_ntop_cache_get_ip(srv, &(con->dst_addr));
	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REMOTE_ADDR"), s, strlen(s));

	if (!buffer_is_empty(con->authed_user)) {
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REMOTE_USER"),
			     CONST_BUF_LEN(con->authed_user));
	}

	if (con->request.content_length > 0 && host->mode != FCGI_AUTHORIZER) {
		/* CGI-SPEC 6.1.2 and FastCGI spec 6.3 */

		/* request.content_length < SSIZE_MAX, see request.c */
		ltostr(buf, con->request.content_length);
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("CONTENT_LENGTH"), buf, strlen(buf));
	}

	if (host->mode != FCGI_AUTHORIZER) {
		/*
		 * SCRIPT_NAME, PATH_INFO and PATH_TRANSLATED according to
		 * http://cgi-spec.golux.com/draft-coar-cgi-v11-03-clean.html
		 * (6.1.14, 6.1.6, 6.1.7)
		 * For AUTHORIZER mode these headers should be omitted.
		 */

		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SCRIPT_NAME"), CONST_BUF_LEN(con->uri.path));

		if (!buffer_is_empty(con->request.pathinfo)) {
			fcgi_env_add(p->fcgi_env, CONST_STR_LEN("PATH_INFO"), CONST_BUF_LEN(con->request.pathinfo));

			/* PATH_TRANSLATED is only defined if PATH_INFO is set */

			if (!buffer_is_empty(host->docroot)) {
				buffer_copy_string_buffer(p->path, host->docroot);
			} else {
				buffer_copy_string_buffer(p->path, con->physical.doc_root);
			}
			buffer_append_string_buffer(p->path, con->request.pathinfo);
			fcgi_env_add(p->fcgi_env, CONST_STR_LEN("PATH_TRANSLATED"), CONST_BUF_LEN(p->path));
		} else {
			fcgi_env_add(p->fcgi_env, CONST_STR_LEN("PATH_INFO"), CONST_STR_LEN(""));
		}
	}

	/*
	 * SCRIPT_FILENAME and DOCUMENT_ROOT for php. The PHP manual
	 * http://www.php.net/manual/en/reserved.variables.php
	 * treatment of PATH_TRANSLATED is different from the one of CGI specs.
	 * TODO: this code should be checked against cgi.fix_pathinfo php
	 * parameter.
	 */

	if (!buffer_is_empty(host->docroot)) {
		/*
		 * rewrite SCRIPT_FILENAME
		 *
		 */

		buffer_copy_string_buffer(p->path, host->docroot);
		buffer_append_string_buffer(p->path, con->uri.path);

		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SCRIPT_FILENAME"), CONST_BUF_LEN(p->path));
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("DOCUMENT_ROOT"), CONST_BUF_LEN(host->docroot));
	} else {
		buffer_copy_string_buffer(p->path, con->physical.path);

		/* cgi.fix_pathinfo need a broken SCRIPT_FILENAME to find out what PATH_INFO is itself
		 *
		 * see src/sapi/cgi_main.c, init_request_info()
		 */
		if (host->break_scriptfilename_for_php) {
			buffer_append_string_buffer(p->path, con->request.pathinfo);
		}

		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SCRIPT_FILENAME"), CONST_BUF_LEN(p->path));
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("DOCUMENT_ROOT"), CONST_BUF_LEN(con->physical.doc_root));
	}

	if (host->strip_request_uri->used > 1) {
		/* we need at least one char to strip off */
		/**
		 * /app1/index/list
		 *
		 * stripping /app1 or /app1/ should lead to
		 *
		 * /index/list
		 *
		 */
		if ('/' != host->strip_request_uri->ptr[host->strip_request_uri->used - 2]) {
			/* fix the user-input to have / as last char */
			buffer_append_string(host->strip_request_uri, "/");
		}

		if (con->request.orig_uri->used >= host->strip_request_uri->used &&
		    0 == strncmp(con->request.orig_uri->ptr, host->strip_request_uri->ptr, host->strip_request_uri->used - 1)) {
			/* the left is the same */

			fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REQUEST_URI"),
					con->request.orig_uri->ptr + (host->strip_request_uri->used - 2),
					con->request.orig_uri->used - (host->strip_request_uri->used - 2));
		} else {
			fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REQUEST_URI"), CONST_BUF_LEN(con->request.orig_uri));
		}
	} else {
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REQUEST_URI"), CONST_BUF_LEN(con->request.orig_uri));
	}
	if (!buffer_is_equal(con->request.uri, con->request.orig_uri)) {
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REDIRECT_URI"), CONST_BUF_LEN(con->request.uri));
	}
	if (!buffer_is_empty(con->uri.query)) {
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("QUERY_STRING"), CONST_BUF_LEN(con->uri.query));
	} else {
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("QUERY_STRING"), CONST_STR_LEN(""));
	}

	s = get_http_method_name(con->request.http_method);
	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REQUEST_METHOD"), s, strlen(s));
	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("REDIRECT_STATUS"), CONST_STR_LEN("200")); /* if php is compiled with --force-redirect */
	s = get_http_version_name(con->request.http_version);
	fcgi_env_add(p->fcgi_env, CONST_STR_LEN("SERVER_PROTOCOL"), s, strlen(s));

#ifdef USE_OPENSSL
	if (srv_sock->is_ssl) {
		fcgi_env_add(p->fcgi_env, CONST_STR_LEN("HTTPS"), CONST_STR_LEN("on"));
	}
#endif


	fcgi_env_add_request_headers(srv, con, p);

	fcgi_header(&(header), FCGI_PARAMS, request_id, p->fcgi_env->used, 0);
	buffer_append_memory(b, (const char *)&header, sizeof(header));
	buffer_append_memory(b, (const char *)p->fcgi_env->ptr, p->fcgi_env->used);

	fcgi_header(&(header), FCGI_PARAMS, request_id, 0, 0);
	buffer_append_memory(b, (const char *)&header, sizeof(header));

	b->used++; /* add virtual \0 */
	hctx->wb->bytes_in += b->used - 1;

	if (con->request.content_length) {
		chunkqueue *req_cq = con->recv;
		chunk *req_c;
		off_t offset;

		/* something to send ? */
		for (offset = 0, req_c = req_cq->first; offset != req_cq->bytes_in; ) {
			off_t weWant = req_cq->bytes_in - offset > FCGI_MAX_LENGTH ? FCGI_MAX_LENGTH : req_cq->bytes_in - offset;
			off_t written = 0;
			off_t weHave = 0;

			/* we announce toWrite octects
			 * now take all the request_content chunk that we need to fill this request
			 * */

			b = chunkqueue_get_append_buffer(hctx->wb);
			fcgi_header(&(header), FCGI_STDIN, request_id, weWant, 0);
			buffer_copy_memory(b, (const char *)&header, sizeof(header));
			hctx->wb->bytes_in += sizeof(header);

			if (p->conf.debug > 10) {
				fprintf(stderr, "%s.%d: tosend: %lld / %lld\n", __FILE__, __LINE__, offset, req_cq->bytes_in);
			}

			for (written = 0; written != weWant; ) {
				if (p->conf.debug > 10) {
					fprintf(stderr, "%s.%d: chunk: %lld / %lld\n", __FILE__, __LINE__, written, weWant);
				}

				switch (req_c->type) {
				case FILE_CHUNK:
					weHave = req_c->file.length - req_c->offset;

					if (weHave > weWant - written) weHave = weWant - written;

					if (p->conf.debug > 10) {
						fprintf(stderr, "%s.%d: sending %lld bytes from (%lld / %lld) %s\n",
								__FILE__, __LINE__,
								weHave,
								req_c->offset,
								req_c->file.length,
								req_c->file.name->ptr);
					}

					assert(weHave != 0);

					chunkqueue_append_file(hctx->wb, req_c->file.name, req_c->offset, weHave);

					req_c->offset += weHave;
					req_cq->bytes_out += weHave;
					written += weHave;

					hctx->wb->bytes_in += weHave;

					/* steal the tempfile
					 *
					 * This is tricky:
					 * - we reference the tempfile from the request-content-queue several times
					 *   if the req_c is larger than FCGI_MAX_LENGTH
					 * - we can't simply cleanup the request-content-queue as soon as possible
					 *   as it would remove the tempfiles
					 * - the idea is to 'steal' the tempfiles and attach the is_temp flag to the last
					 *   referencing chunk of the fastcgi-write-queue
					 *
					 *  */

					if (req_c->offset == req_c->file.length) {
						chunk *c;

						if (p->conf.debug > 10) {
							fprintf(stderr, "%s.%d: next chunk\n", __FILE__, __LINE__);
						}
						c = hctx->wb->last;

						assert(c->type == FILE_CHUNK);
						assert(req_c->file.is_temp == 1);

						c->file.is_temp = 1;
						req_c->file.is_temp = 0;

						chunkqueue_remove_finished_chunks(req_cq);

						req_c = req_cq->first;
					}

					break;
				case MEM_CHUNK:
					/* append to the buffer */
					weHave = req_c->mem->used - 1 - req_c->offset;

					if (weHave > weWant - written) weHave = weWant - written;

					buffer_append_memory(b, req_c->mem->ptr + req_c->offset, weHave);

					req_c->offset += weHave;
					req_cq->bytes_out += weHave;
					written += weHave;

					hctx->wb->bytes_in += weHave;

					if (req_c->offset == req_c->mem->used - 1) {
						chunkqueue_remove_finished_chunks(req_cq);

						req_c = req_cq->first;
					}

					break;
				default:
					break;
				}
			}

			b->used++; /* add virtual \0 */
			offset += weWant;
		}
	}

	b = chunkqueue_get_append_buffer(hctx->wb);
	/* terminate STDIN */
	fcgi_header(&(header), FCGI_STDIN, request_id, 0, 0);
	buffer_copy_memory(b, (const char *)&header, sizeof(header));
	b->used++; /* add virtual \0 */

	hctx->wb->bytes_in += sizeof(header);

#if 0
	for (i = 0; i < hctx->write_buffer->used; i++) {
		fprintf(stderr, "%02x ", hctx->write_buffer->ptr[i]);
		if ((i+1) % 16 == 0) {
			size_t j;
			for (j = i-15; j <= i; j++) {
				fprintf(stderr, "%c",
					isprint((unsigned char)hctx->write_buffer->ptr[j]) ? hctx->write_buffer->ptr[j] : '.');
			}
			fprintf(stderr, "\n");
		}
	}
#endif

	return 0;
}

typedef struct {
	buffer  *b;
	size_t   len;
	int      type;
	int      padding;
	size_t   request_id;
} fastcgi_response_packet;

static int fastcgi_get_packet(server *srv, handler_ctx *hctx, fastcgi_response_packet *packet) {
	chunk *	c;
	size_t offset = 0;
	size_t toread = 0;
	FCGI_Header *header;

	if (!hctx->rb->first) return -1;

	packet->b = buffer_init();
	packet->len = 0;
	packet->type = 0;
	packet->padding = 0;
	packet->request_id = 0;

	/* get at least the FastCGI header */
	for (c = hctx->rb->first; c; c = c->next) {
		if (packet->b->used == 0) {
			buffer_copy_string_len(packet->b, c->mem->ptr + c->offset, c->mem->used - c->offset - 1);
		} else {
			buffer_append_string_len(packet->b, c->mem->ptr + c->offset, c->mem->used - c->offset - 1);
		}

		if (packet->b->used >= sizeof(*header) + 1) break;
	}

	if ((packet->b->used == 0) ||
	    (packet->b->used - 1 < sizeof(FCGI_Header))) {
		/* no header */
		buffer_free(packet->b);

		log_error_write(srv, __FILE__, __LINE__, "s", "FastCGI: header too small");
		return -1;
	}

	/* we have at least a header, now check how much me have to fetch */
	header = (FCGI_Header *)(packet->b->ptr);

	packet->len = (header->contentLengthB0 | (header->contentLengthB1 << 8)) + header->paddingLength;
	packet->request_id = (header->requestIdB0 | (header->requestIdB1 << 8));
	packet->type = header->type;
	packet->padding = header->paddingLength;

	/* the first bytes in packet->b are the header */
	offset = sizeof(*header);

	/* ->b should only be the content */
	buffer_copy_string(packet->b, ""); /* used == 1 */

	if (packet->len) {
		/* copy the content */
		for (; c && (packet->b->used < packet->len + 1); c = c->next) {
			size_t weWant = packet->len - (packet->b->used - 1);
			size_t weHave = c->mem->used - c->offset - offset - 1;

			if (weHave > weWant) weHave = weWant;

			buffer_append_string_len(packet->b, c->mem->ptr + c->offset + offset, weHave);

			/* we only skipped the first 8 bytes as they are the fcgi header */
			offset = 0;
		}

		if (packet->b->used < packet->len + 1) {
			/* we didn't got the full packet */

			buffer_free(packet->b);
			return -1;
		}

		packet->b->used -= packet->padding;
		packet->b->ptr[packet->b->used - 1] = '\0';
	}

	/* tag the chunks as read */
	toread = packet->len + sizeof(FCGI_Header);
	for (c = hctx->rb->first; c && toread; c = c->next) {
		if (c->mem->used - c->offset - 1 <= toread) {
			/* we read this whole buffer, move it to unused */
			toread -= c->mem->used - c->offset - 1;
			c->offset = c->mem->used - 1; /* everthing has been written */
		} else {
			c->offset += toread;
			toread = 0;
		}
	}

	chunkqueue_remove_finished_chunks(hctx->rb);

	return 0;
}

static int fcgi_demux_response(server *srv, handler_ctx *hctx) {
	int fin = 0;

	plugin_data *p    = hctx->plugin_data;
	connection *con   = hctx->remote_conn;
	fcgi_extension_host *host= hctx->host;
	fcgi_proc *proc   = hctx->proc;
	handler_t ret;

	/* in case we read nothing, check the return code
	 * if we got something, be happy :)
	 *
	 * Ok, to be honest:
	 * - it is fine to receive a EAGAIN on a second read() call
	 * - it might be fine they we get a con-close on a second read() call */
	switch(srv->network_backend_read(srv, con, hctx->sock, hctx->rb)) {
	case NETWORK_STATUS_WAIT_FOR_EVENT:
		/* a EAGAIN after we read exactly the chunk-size */

		ERROR("%s", "oops, got a EAGAIN even if we just got call for the event, wired");
		return -1;
	case NETWORK_STATUS_SUCCESS:
		break;
	default:
		ERROR("reading from fastcgi socket failed (fd=%d)", hctx->sock->fd);
		return -1;
	}

	/*
	 * parse the fastcgi packets and forward the content to the write-queue
	 *
	 */
	while (fin == 0) {
		fastcgi_response_packet packet;

		/* check if we have at least one packet */
		if (0 != fastcgi_get_packet(srv, hctx, &packet)) {
			/* no full packet */
			break;
		}

		switch(packet.type) {
		case FCGI_STDOUT:
			if (packet.len == 0) break;

			/* is the header already finished */
			if (0 == con->file_started) {
				int have_content_length = 0;
				int need_more = 0;
				size_t i;

				/* append the current packet to the chunk queue */
				chunkqueue_append_buffer(hctx->http_rb, packet.b);
				http_response_reset(p->resp);

				switch(http_response_parse_cq(hctx->http_rb, p->resp)) {
				case PARSE_ERROR:
					/* parsing the response header failed */

					con->http_status = 502; /* Bad Gateway */

					return 1;
				case PARSE_NEED_MORE:
					need_more = 1;
					break; /* leave the loop */
				case PARSE_SUCCESS:
					break;
				default:
					/* should not happen */
					SEGFAULT();
				}

				if (need_more) break;

				chunkqueue_remove_finished_chunks(hctx->http_rb);

				con->http_status = p->resp->status;
				hctx->send_content_body = 1;

				/* handle the header fields */
				if (host->mode == FCGI_AUTHORIZER) {
					/* auth mode is a bit different */

    					if (con->http_status == 0 ||
					    con->http_status == 200) {
						/* a authorizer with approved the static request, ignore the content here */
						hctx->send_content_body = 0;
					}
				}

				/* copy the http-headers */
				for (i = 0; i < p->resp->headers->used; i++) {
					const char *ign[] = { "Status", NULL };
					size_t j;
					data_string *ds;

					data_string *header = (data_string *)p->resp->headers->data[i];

					/* ignore all headers in AUTHORIZER mode */
					if (host->mode == FCGI_AUTHORIZER) continue;

					/* some headers are ignored by default */
					for (j = 0; ign[j]; j++) {
						if (0 == strcasecmp(ign[j], header->key->ptr)) break;
					}
					if (ign[j]) continue;

					if (0 == buffer_caseless_compare(CONST_BUF_LEN(header->key), CONST_STR_LEN("Location"))) {
						/* CGI/1.1 rev 03 - 7.2.1.2 */
						con->http_status = 302;
					} else if (0 == buffer_caseless_compare(CONST_BUF_LEN(header->key), CONST_STR_LEN("Content-Length"))) {
						have_content_length = 1;
					} else if (0 == buffer_caseless_compare(CONST_BUF_LEN(header->key), CONST_STR_LEN("X-Sendfile")) ||
						   0 == buffer_caseless_compare(CONST_BUF_LEN(header->key), CONST_STR_LEN("X-LIGHTTPD-send-file"))) {

						stat_cache_entry *sce;

						if (host->allow_xsendfile &&
						    HANDLER_ERROR != stat_cache_get_entry(srv, con, header->value, &sce)) {
							chunkqueue_append_file(con->send, header->value, 0, sce->st.st_size);
							hctx->send_content_body = 0; /* ignore the content */

							joblist_append(srv, con);
						}

						continue; /* ignore header */
					}

					if (NULL == (ds = (data_string *)array_get_unused_element(con->response.headers, TYPE_STRING))) {
						ds = data_response_init();
					}
					buffer_copy_string_buffer(ds->key, header->key);
					buffer_copy_string_buffer(ds->value, header->value);

					array_insert_unique(con->response.headers, (data_unset *)ds);
				}

				/* header is complete ... go on with the body */

				con->file_started = 1;

				if (hctx->send_content_body) {
					chunk *c = hctx->http_rb->first;

					/* copy the rest of the data */
					for (c = hctx->http_rb->first; c; c = c->next) {
						if (c->mem->used > 1) {
							chunkqueue_append_mem(con->send, c->mem->ptr + c->offset, c->mem->used - c->offset);
							c->offset = c->mem->used - 1;
						}
					}
					chunkqueue_remove_finished_chunks(hctx->http_rb);
					joblist_append(srv, con);
				}
			} else if (hctx->send_content_body && packet.b->used > 1) {
				chunkqueue_append_mem(con->send, packet.b->ptr, packet.b->used);
				joblist_append(srv, con);
			}
			break;
		case FCGI_STDERR:
			if (packet.len == 0) break;

			log_error_write(srv, __FILE__, __LINE__, "sb",
					"FastCGI-stderr:", packet.b);

			break;
		case FCGI_END_REQUEST:
			con->send->is_closed = 1;

			if (host->mode != FCGI_AUTHORIZER ||
			    !(con->http_status == 0 ||
			      con->http_status == 200)) {
				/* send chunk-end if nesseary */
				joblist_append(srv, con);
			}

			fin = 1;
			break;
		default:
			log_error_write(srv, __FILE__, __LINE__, "sd",
					"FastCGI: header.type not handled: ", packet.type);
			break;
		}
		buffer_free(packet.b);
	}

	return fin;
}

static int fcgi_restart_dead_procs(server *srv, plugin_data *p, fcgi_extension_host *host) {
	fcgi_proc *proc;

	for (proc = host->first; proc; proc = proc->next) {
		int status;

		if (p->conf.debug > 2) {
			log_error_write(srv, __FILE__, __LINE__,  "sbdddd",
					"proc:",
					proc->connection_name,
					proc->state,
					proc->is_local,
					proc->load,
					proc->pid);
		}

		/*
		 * if the remote side is overloaded, we check back after <n> seconds
		 *
		 */
		switch (proc->state) {
		case PROC_STATE_KILLED:
		case PROC_STATE_UNSET:
			/* this should never happen as long as adaptive spawing is disabled */
			assert(0);

			break;
		case PROC_STATE_RUNNING:
			break;
		case PROC_STATE_OVERLOADED:
			if (srv->cur_ts <= proc->disabled_until) break;

			proc->state = PROC_STATE_RUNNING;
			host->active_procs++;

			log_error_write(srv, __FILE__, __LINE__,  "sbdb",
					"fcgi-server re-enabled:",
					host->host, host->port,
					host->unixsocket);
			break;
		case PROC_STATE_DIED_WAIT_FOR_PID:
			/* non-local procs don't have PIDs to wait for */
			if (!proc->is_local) break;

			/* the child should not terminate at all */
#ifndef _WIN32
			switch(waitpid(proc->pid, &status, WNOHANG)) {
			case 0:
				/* child is still alive */
				break;
			case -1:
				break;
			default:
				if (WIFEXITED(status)) {
#if 0
					log_error_write(srv, __FILE__, __LINE__, "sdsd",
							"child exited, pid:", proc->pid,
							"status:", WEXITSTATUS(status));
#endif
				} else if (WIFSIGNALED(status)) {
					log_error_write(srv, __FILE__, __LINE__, "sd",
							"child signaled:",
							WTERMSIG(status));
				} else {
					log_error_write(srv, __FILE__, __LINE__, "sd",
							"child died somehow:",
							status);
				}

				proc->state = PROC_STATE_DIED;
				break;
			}
#endif
			/* fall through if we have a dead proc now */
			if (proc->state != PROC_STATE_DIED) break;

		case PROC_STATE_DIED:
			/* local proc get restarted by us,
			 * remote ones hopefully by the admin */

			if (proc->is_local) {
				/* we still have connections bound to this proc,
				 * let them terminate first */
				if (proc->load != 0) break;

				/* restart the child */

				if (p->conf.debug) {
					log_error_write(srv, __FILE__, __LINE__, "ssbsdsd",
							"--- fastcgi spawning",
							"\n\tsocket", proc->connection_name,
							"\n\tcurrent:", 1, "/", host->min_procs);
				}

				if (fcgi_spawn_connection(srv, p, host, proc)) {
					log_error_write(srv, __FILE__, __LINE__, "s",
							"ERROR: spawning fcgi failed.");
					return HANDLER_ERROR;
				}
			} else {
				if (srv->cur_ts <= proc->disabled_until) break;

				proc->state = PROC_STATE_RUNNING;
				host->active_procs++;

				log_error_write(srv, __FILE__, __LINE__,  "sb",
						"fcgi-server re-enabled:",
						proc->connection_name);
			}
			break;
		}
	}

	return 0;
}

static handler_t fcgi_write_request(server *srv, handler_ctx *hctx) {
	plugin_data *p    = hctx->plugin_data;
	fcgi_extension_host *host= hctx->host;
	connection *con   = hctx->remote_conn;
	fcgi_proc  *proc;

	int ret;

	/* sanity check */
	if (!host ||
	    ((!host->host->used || !host->port) && !host->unixsocket->used)) {
		log_error_write(srv, __FILE__, __LINE__, "sxddd",
				"write-req: error",
				host,
				host->host->used,
				host->port,
				host->unixsocket->used);

		hctx->proc->disabled_until = srv->cur_ts + 10;
		hctx->proc->state = PROC_STATE_DIED;

		return HANDLER_ERROR;
	}

	/* we can't handle this in the switch as we have to fall through in it */
	if (hctx->state == FCGI_STATE_CONNECT_DELAYED) {
		int socket_error;
		socklen_t socket_error_len = sizeof(socket_error);

		/* try to finish the connect() */
		if (0 != getsockopt(hctx->sock->fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len)) {
			log_error_write(srv, __FILE__, __LINE__, "ss",
					"getsockopt failed:", strerror(errno));

			hctx->proc->disabled_until = srv->cur_ts + 10;
			hctx->proc->state = PROC_STATE_DIED;

			return HANDLER_ERROR;
		}
		if (socket_error != 0) {
			if (!hctx->proc->is_local || p->conf.debug) {
				/* local procs get restarted */

				log_error_write(srv, __FILE__, __LINE__, "sssb",
						"establishing connection failed:", strerror(socket_error),
						"socket:", hctx->proc->connection_name);
			}

			hctx->proc->disabled_until = srv->cur_ts + 5;

			if (hctx->proc->is_local) {
				hctx->proc->state = PROC_STATE_DIED_WAIT_FOR_PID;
			} else {
				hctx->proc->state = PROC_STATE_DIED;
			}

			hctx->proc->state = PROC_STATE_DIED;

			fastcgi_status_copy_procname(p->statuskey, hctx->host, hctx->proc);
			buffer_append_string(p->statuskey, ".died");

			status_counter_inc(CONST_BUF_LEN(p->statuskey));

			return HANDLER_ERROR;
		}
		/* go on with preparing the request */
		hctx->state = FCGI_STATE_PREPARE_WRITE;
	}


	switch(hctx->state) {
	case FCGI_STATE_CONNECT_DELAYED:
		/* should never happen */
		break;
	case FCGI_STATE_INIT:
		/* do we have a running process for this host (max-procs) ? */
		hctx->proc = NULL;

		for (proc = hctx->host->first;
		     proc && proc->state != PROC_STATE_RUNNING;
		     proc = proc->next);

		/* all childs are dead */
		if (proc == NULL) {
			hctx->sock->fde_ndx = -1;

			return HANDLER_ERROR;
		}

		hctx->proc = proc;

		/* check the other procs if they have a lower load */
		for (proc = proc->next; proc; proc = proc->next) {
			if (proc->state != PROC_STATE_RUNNING) continue;
			if (proc->load < hctx->proc->load) hctx->proc = proc;
		}

		ret = host->unixsocket->used ? AF_UNIX : AF_INET;

		if (-1 == (hctx->sock->fd = socket(ret, SOCK_STREAM, 0))) {
			if (errno == EMFILE ||
			    errno == EINTR) {
				log_error_write(srv, __FILE__, __LINE__, "sd",
						"wait for fd at connection:", con->sock->fd);

				return HANDLER_WAIT_FOR_FD;
			}

			log_error_write(srv, __FILE__, __LINE__, "ssdd",
					"socket failed:", strerror(errno), srv->cur_fds, srv->max_fds);
			return HANDLER_ERROR;
		}
		hctx->sock->fde_ndx = -1;

		srv->cur_fds++;

		fdevent_register(srv->ev, hctx->sock, fcgi_handle_fdevent, hctx);

		if (-1 == fdevent_fcntl_set(srv->ev, hctx->sock)) {
			log_error_write(srv, __FILE__, __LINE__, "ss",
					"fcntl failed:", strerror(errno));

			return HANDLER_ERROR;
		}

		if (hctx->proc->is_local) {
			hctx->pid = hctx->proc->pid;
		}

		switch (fcgi_establish_connection(srv, hctx)) {
		case CONNECTION_DELAYED:
			/* connection is in progress, wait for an event and call getsockopt() below */

			fdevent_event_add(srv->ev, hctx->sock, FDEVENT_OUT);

			fcgi_set_state(srv, hctx, FCGI_STATE_CONNECT_DELAYED);
			return HANDLER_WAIT_FOR_EVENT;
		case CONNECTION_OVERLOADED:
			/* cool down the backend, it is overloaded
			 * -> EAGAIN */

			log_error_write(srv, __FILE__, __LINE__, "ssdsd",
				"backend is overloaded, we disable it for a 2 seconds and send the request to another backend instead:",
				"reconnects:", hctx->reconnects,
				"load:", host->load);


			hctx->proc->disabled_until = srv->cur_ts + 2;
			hctx->proc->state = PROC_STATE_OVERLOADED;

			fastcgi_status_copy_procname(p->statuskey, hctx->host, hctx->proc);
			buffer_append_string(p->statuskey, ".overloaded");

			status_counter_inc(CONST_BUF_LEN(p->statuskey));

			return HANDLER_ERROR;
		case CONNECTION_DEAD:
			/* we got a hard error from the backend like
			 * - ECONNREFUSED for tcp-ip sockets
			 * - ENOENT for unix-domain-sockets
			 *
			 * for check if the host is back in 5 seconds
			 *  */

			hctx->proc->disabled_until = srv->cur_ts + 5;
			if (hctx->proc->is_local) {
				hctx->proc->state = PROC_STATE_DIED_WAIT_FOR_PID;
			} else {
				hctx->proc->state = PROC_STATE_DIED;
			}

			log_error_write(srv, __FILE__, __LINE__, "ssdsd",
				"backend died, we disable it for a 5 seconds and send the request to another backend instead:",
				"reconnects:", hctx->reconnects,
				"load:", host->load);

			fastcgi_status_copy_procname(p->statuskey, hctx->host, hctx->proc);
			buffer_append_string(p->statuskey, ".died");

			status_counter_inc(CONST_BUF_LEN(p->statuskey));

			return HANDLER_ERROR;
		case CONNECTION_OK:
			/* everything is ok, go on */

			fcgi_set_state(srv, hctx, FCGI_STATE_PREPARE_WRITE);

			break;
		case CONNECTION_UNSET:
			break;
		}

	case FCGI_STATE_PREPARE_WRITE:
		/* ok, we have the connection */

		hctx->proc->load++;
		hctx->proc->last_used = srv->cur_ts;
		hctx->got_proc = 1;

		status_counter_inc(CONST_STR_LEN("fastcgi.requests"));
		status_counter_inc(CONST_STR_LEN("fastcgi.active-requests"));

		fastcgi_status_copy_procname(p->statuskey, hctx->host, hctx->proc);
		buffer_append_string(p->statuskey, ".connected");

		status_counter_inc(CONST_BUF_LEN(p->statuskey));

		/* the proc-load */
		fastcgi_status_copy_procname(p->statuskey, hctx->host, hctx->proc);
		buffer_append_string(p->statuskey, ".load");

		status_counter_set(CONST_BUF_LEN(p->statuskey), hctx->proc->load);

		/* the host-load */
		fastcgi_status_copy_procname(p->statuskey, hctx->host, NULL);
		buffer_append_string(p->statuskey, ".load");

		status_counter_set(CONST_BUF_LEN(p->statuskey), hctx->host->load);

		if (p->conf.debug) {
			log_error_write(srv, __FILE__, __LINE__, "ssdsbsd",
					"got proc:",
					"pid:", hctx->proc->pid,
					"socket:", hctx->proc->connection_name,
					"load:", hctx->proc->load);
		}

		/* move the proc-list entry down the list */
		if (hctx->request_id == 0) {
			hctx->request_id = fcgi_requestid_new(srv, p);
		} else {
			log_error_write(srv, __FILE__, __LINE__, "sd",
					"fcgi-request is already in use:", hctx->request_id);
		}

		/* fall through */
		fcgi_create_env(srv, hctx, hctx->request_id);

		fcgi_set_state(srv, hctx, FCGI_STATE_WRITE);

		/* fall through */
	case FCGI_STATE_WRITE:
		ret = srv->network_backend_write(srv, con, hctx->sock, hctx->wb);

		chunkqueue_remove_finished_chunks(hctx->wb);

		if (ret < 0) {
			switch(errno) {
			case ENOTCONN:
				/* the connection got dropped after accept()
				 *
				 * this is most of the time a PHP which dies
				 * after PHP_FCGI_MAX_REQUESTS
				 *
				 */
				if (hctx->wb->bytes_out == 0 &&
				    hctx->reconnects < 5) {
#ifndef _WIN32
					usleep(10000); /* take away the load of the webserver
							* to let the php a chance to restart
							*/
#endif
					fcgi_reconnect(srv, hctx);

					return HANDLER_WAIT_FOR_FD;
				}

				/* not reconnected ... why
				 *
				 * far@#lighttpd report this for FreeBSD
				 *
				 */

				log_error_write(srv, __FILE__, __LINE__, "ssosd",
						"[REPORT ME] connection was dropped after accept(). reconnect() denied:",
						"write-offset:", hctx->wb->bytes_out,
						"reconnect attempts:", hctx->reconnects);

				return HANDLER_ERROR;
			case EAGAIN:
			case EINTR:
				fdevent_event_add(srv->ev, hctx->sock, FDEVENT_OUT);

				return HANDLER_WAIT_FOR_EVENT;
			default:
				log_error_write(srv, __FILE__, __LINE__, "ssd",
						"write failed:", strerror(errno), errno);

				return HANDLER_ERROR;
			}
		}

		if (hctx->wb->bytes_out == hctx->wb->bytes_in) {
			/* we don't need the out event anymore */
			fdevent_event_del(srv->ev, hctx->sock);
			fdevent_event_add(srv->ev, hctx->sock, FDEVENT_IN);
			fcgi_set_state(srv, hctx, FCGI_STATE_READ);
		} else {
			fdevent_event_add(srv->ev, hctx->sock, FDEVENT_OUT);

			return HANDLER_WAIT_FOR_EVENT;
		}

		break;
	case FCGI_STATE_READ:
		/* waiting for a response */
		break;
	default:
		log_error_write(srv, __FILE__, __LINE__, "s", "(debug) unknown state");
		return HANDLER_ERROR;
	}

	return HANDLER_WAIT_FOR_EVENT;
}


/* might be called on fdevent after a connect() is delay too
 * */
SUBREQUEST_FUNC(mod_fastcgi_handle_subrequest) {
	plugin_data *p = p_d;

	handler_ctx *hctx = con->plugin_ctx[p->id];
	fcgi_proc *proc;
	fcgi_extension_host *host;

	if (NULL == hctx) return HANDLER_GO_ON;

	/* not my job */
	if (con->mode != p->id) return HANDLER_GO_ON;

	/* we don't have a host yet, choose one
	 * -> this happens in the first round
	 *    and when the host died and we have to select a new one */
	if (hctx->host == NULL) {
		size_t k;
		int ndx, used = -1;

		/* get best server */
		for (k = 0, ndx = -1; k < hctx->ext->used; k++) {
			host = hctx->ext->hosts[k];

			/* we should have at least one proc that can do something */
			if (host->active_procs == 0) continue;

			if (used == -1 || host->load < used) {
				used = host->load;

				ndx = k;
			}
		}

		/* found a server */
		if (ndx == -1) {
			/* all hosts are down */

			fcgi_connection_close(srv, hctx);

			con->http_status = 500;
			con->mode = DIRECT;

			return HANDLER_FINISHED;
		}

		host = hctx->ext->hosts[ndx];

		/*
		 * if check-local is disabled, use the uri.path handler
		 *
		 */

		/* init handler-context */
		hctx->host = host;

		/* we put a connection on this host, move the other new connections to other hosts
		 *
		 * as soon as hctx->host is unassigned, decrease the load again */
		hctx->host->load++;
		hctx->proc = NULL;
	} else {
		host = hctx->host;
	}

	/* ok, create the request */
	switch(fcgi_write_request(srv, hctx)) {
	case HANDLER_ERROR:
		proc = hctx->proc;
		host = hctx->host;

		if (hctx->state == FCGI_STATE_INIT ||
		    hctx->state == FCGI_STATE_CONNECT_DELAYED) {
			if (proc) host->active_procs--;

			fcgi_restart_dead_procs(srv, p, host);

			/* cleanup this request and let the request handler start this request again */
			if (hctx->reconnects < 5) {
				fcgi_reconnect(srv, hctx);
				joblist_append(srv, con); /* in case we come from the event-handler */

				return HANDLER_WAIT_FOR_FD;
			} else {
				fcgi_connection_close(srv, hctx);

				buffer_reset(con->physical.path);
				con->mode = DIRECT;
				con->http_status = 500;
				joblist_append(srv, con); /* in case we come from the event-handler */

				return HANDLER_FINISHED;
			}
		} else {
			fcgi_connection_close(srv, hctx);

			buffer_reset(con->physical.path);
			con->mode = DIRECT;
			con->http_status = 503;
			joblist_append(srv, con); /* really ? */

			return HANDLER_FINISHED;
		}
	case HANDLER_WAIT_FOR_EVENT:
		if (con->file_started == 1) {
			return HANDLER_FINISHED;
		} else {
			return HANDLER_WAIT_FOR_EVENT;
		}
	case HANDLER_WAIT_FOR_FD:
		return HANDLER_WAIT_FOR_FD;
	default:
		log_error_write(srv, __FILE__, __LINE__, "s", "subrequest write-req default");
		return HANDLER_ERROR;
	}
}

static handler_t fcgi_handle_fdevent(void *s, void *ctx, int revents) {
	server      *srv  = (server *)s;
	handler_ctx *hctx = ctx;
	connection  *con  = hctx->remote_conn;
	plugin_data *p    = hctx->plugin_data;

	fcgi_proc *proc   = hctx->proc;
	fcgi_extension_host *host= hctx->host;

	if ((revents & FDEVENT_IN) &&
	    hctx->state == FCGI_STATE_READ) {
		switch (fcgi_demux_response(srv, hctx)) {
		case 0:
			break;
		case 1:

			if (host->mode == FCGI_AUTHORIZER &&
		   	    (con->http_status == 200 ||
			     con->http_status == 0)) {
				/*
				 * If we are here in AUTHORIZER mode then a request for autorizer
				 * was proceeded already, and status 200 has been returned. We need
				 * now to handle autorized request.
				 */

				buffer_copy_string_buffer(con->physical.doc_root, host->docroot);

				buffer_copy_string_buffer(con->physical.path, host->docroot);
				buffer_append_string_buffer(con->physical.path, con->uri.path);
				fcgi_connection_close(srv, hctx);

				con->mode = DIRECT;
				con->file_started = 1; /* fcgi_extension won't touch the request afterwards */
			} else {
				/* we are done */
				fcgi_connection_close(srv, hctx);
			}

			joblist_append(srv, con);
			return HANDLER_FINISHED;
		case -1:
			if (proc->pid && proc->state != PROC_STATE_DIED) {
				int status;

				/* only fetch the zombie if it is not already done */
#ifndef _WIN32
				switch(waitpid(proc->pid, &status, WNOHANG)) {
				case 0:
					/* child is still alive */
					break;
				case -1:
					break;
				default:
					/* the child should not terminate at all */
					if (WIFEXITED(status)) {
						log_error_write(srv, __FILE__, __LINE__, "sdsd",
								"child exited, pid:", proc->pid,
								"status:", WEXITSTATUS(status));
					} else if (WIFSIGNALED(status)) {
						log_error_write(srv, __FILE__, __LINE__, "sd",
								"child signaled:",
								WTERMSIG(status));
					} else {
						log_error_write(srv, __FILE__, __LINE__, "sd",
								"child died somehow:",
								status);
					}

					if (p->conf.debug) {
						log_error_write(srv, __FILE__, __LINE__, "ssbsdsd",
								"--- fastcgi spawning",
								"\n\tsocket", proc->connection_name,
								"\n\tcurrent:", 1, "/", host->min_procs);
					}

					if (fcgi_spawn_connection(srv, p, host, proc)) {
						/* respawning failed, retry later */
						proc->state = PROC_STATE_DIED;

						log_error_write(srv, __FILE__, __LINE__, "s",
								"respawning failed, will retry later");
					}

					break;
				}
#endif
			}

			if (con->file_started == 0) {
				/* nothing has been send out yet, try to use another child */

				if (hctx->wb->bytes_out == 0 &&
				    hctx->reconnects < 5) {
					fcgi_reconnect(srv, hctx);

					log_error_write(srv, __FILE__, __LINE__, "ssbsbs",
						"response not received, request not sent",
						"on socket:", proc->connection_name,
						"for", con->uri.path, ", reconnecting");

					return HANDLER_WAIT_FOR_FD;
				}

				log_error_write(srv, __FILE__, __LINE__, "sosbsbs",
						"response not received, request sent:", hctx->wb->bytes_out,
						"on socket:", proc->connection_name,
						"for", con->uri.path, ", closing connection");

				fcgi_connection_close(srv, hctx);

				buffer_reset(con->physical.path);
				con->http_status = 500;
				con->mode = DIRECT;
			} else {
				/* response might have been already started, kill the connection */
				fcgi_connection_close(srv, hctx);

				log_error_write(srv, __FILE__, __LINE__, "ssbsbs",
						"response already sent out, but backend returned error",
						"on socket:", proc->connection_name,
						"for", con->uri.path, ", terminating connection");

				connection_set_state(srv, con, CON_STATE_ERROR);
			}

			/* */


			joblist_append(srv, con);
			return HANDLER_FINISHED;
		}
	}

	if (revents & FDEVENT_OUT) {
		if (hctx->state == FCGI_STATE_CONNECT_DELAYED ||
		    hctx->state == FCGI_STATE_WRITE) {
			/* we are allowed to send something out
			 *
			 * 1. in a unfinished connect() call
			 * 2. in a unfinished write() call (long POST request)
			 */
			return mod_fastcgi_handle_subrequest(srv, con, p);
		} else {
			log_error_write(srv, __FILE__, __LINE__, "sd",
					"got a FDEVENT_OUT and didn't know why:",
					hctx->state);
		}
	}

	/* perhaps this issue is already handled */
	if (revents & FDEVENT_HUP) {
		if (hctx->state == FCGI_STATE_CONNECT_DELAYED) {
			/* getoptsock will catch this one (right ?)
			 *
			 * if we are in connect we might get a EINPROGRESS
			 * in the first call and a FDEVENT_HUP in the
			 * second round
			 *
			 * FIXME: as it is a bit ugly.
			 *
			 */
			return mod_fastcgi_handle_subrequest(srv, con, p);
		} else if (hctx->state == FCGI_STATE_READ &&
			   hctx->proc->port == 0) {
			/* FIXME:
			 *
			 * ioctl says 8192 bytes to read from PHP and we receive directly a HUP for the socket
			 * even if the FCGI_FIN packet is not received yet
			 */
		} else {
			log_error_write(srv, __FILE__, __LINE__, "sbSBSDSd",
					"error: unexpected close of fastcgi connection for",
					con->uri.path,
					"(no fastcgi process on host:",
					host->host,
					", port: ",
					host->port,
					" ?)",
					hctx->state);

			connection_set_state(srv, con, CON_STATE_ERROR);
			fcgi_connection_close(srv, hctx);
			joblist_append(srv, con);
		}
	} else if (revents & FDEVENT_ERR) {
		log_error_write(srv, __FILE__, __LINE__, "s",
				"fcgi: got a FDEVENT_ERR. Don't know why.");
		/* kill all connections to the fastcgi process */


		connection_set_state(srv, con, CON_STATE_ERROR);
		fcgi_connection_close(srv, hctx);
		joblist_append(srv, con);
	}

	return HANDLER_FINISHED;
}

static int fcgi_patch_connection(server *srv, connection *con, plugin_data *p) {
	size_t i, j;
	plugin_config *s = p->config_storage[0];

	PATCH_OPTION(exts);
	PATCH_OPTION(debug);
	PATCH_OPTION(ext_mapping);

	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		s = p->config_storage[i];

		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;

		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];

			if (buffer_is_equal_string(du->key, CONST_STR_LEN("fastcgi.server"))) {
				PATCH_OPTION(exts);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("fastcgi.debug"))) {
				PATCH_OPTION(debug);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("fastcgi.map-extensions"))) {
				PATCH_OPTION(ext_mapping);
			}
		}
	}

	return 0;
}

static handler_t fcgi_check_extension(server *srv, connection *con, void *p_d, int uri_path_handler) {
	plugin_data *p = p_d;
	size_t s_len;
	size_t k;
	buffer *fn;
	fcgi_extension *extension = NULL;
	fcgi_extension_host *host = NULL;

	/* Possibly, we processed already this request */
	if (con->file_started == 1) return HANDLER_GO_ON;

	fn = uri_path_handler ? con->uri.path : con->physical.path;

	if (buffer_is_empty(fn)) return HANDLER_GO_ON;

	s_len = fn->used - 1;

	fcgi_patch_connection(srv, con, p);

	/* fastcgi.map-extensions maps extensions to existing fastcgi.server entries
	 *
	 * fastcgi.map-extensions = ( ".php3" => ".php" )
	 *
	 * fastcgi.server = ( ".php" => ... )
	 *
	 * */

	/* check if extension-mapping matches */
	for (k = 0; k < p->conf.ext_mapping->used; k++) {
		data_string *ds = (data_string *)p->conf.ext_mapping->data[k];
		size_t ct_len; /* length of the config entry */

		if (ds->key->used == 0) continue;

		ct_len = ds->key->used - 1;

		if (s_len < ct_len) continue;

		/* found a mapping */
		if (0 == strncmp(fn->ptr + s_len - ct_len, ds->key->ptr, ct_len)) {
			/* check if we know the extension */

			/* we can reuse k here */
			for (k = 0; k < p->conf.exts->used; k++) {
				extension = p->conf.exts->exts[k];

				if (buffer_is_equal(ds->value, extension->key)) {
					break;
				}
			}

			if (k == p->conf.exts->used) {
				/* found nothign */
				extension = NULL;
			}
			break;
		}
	}

	if (extension == NULL) {
		/* check if extension matches */
		for (k = 0; k < p->conf.exts->used; k++) {
			size_t ct_len; /* length of the config entry */

			extension = p->conf.exts->exts[k];

			if (extension->key->used == 0) continue;

			ct_len = extension->key->used - 1;

			if (s_len < ct_len) continue;

			/* check extension in the form "/fcgi_pattern" */
			if (*(extension->key->ptr) == '/' && strncmp(fn->ptr, extension->key->ptr, ct_len) == 0) {
				break;
			} else if (0 == strncmp(fn->ptr + s_len - ct_len, extension->key->ptr, ct_len)) {
				/* check extension in the form ".fcg" */
				break;
			}
		}
		/* extension doesn't match */
		if (k == p->conf.exts->used) {
			return HANDLER_GO_ON;
		}
	}

	/* check if we have at least one server for this extension up and running */
	for (k = 0; k < extension->used; k++) {
		host = extension->hosts[k];

		/* we should have at least one proc that can do somthing */
		if (host->active_procs == 0) {
			host = NULL;

			continue;
		}

		/* we found one host that is alive */
		break;
	}

	if (!host) {
		/* sorry, we don't have a server alive for this ext */
		buffer_reset(con->physical.path);
		con->http_status = 500;

		/* only send the 'no handler' once */
		if (!extension->note_is_sent) {
			extension->note_is_sent = 1;

			log_error_write(srv, __FILE__, __LINE__, "sbsbs",
					"all handlers for ", con->uri.path,
					"on", extension->key,
					"are down.");
		}

		return HANDLER_FINISHED;
	}

	/* a note about no handler is not sent yey */
	extension->note_is_sent = 0;

	/*
	 * if check-local is disabled, use the uri.path handler
	 *
	 */

	/* init handler-context */
	if (uri_path_handler) {
		if (host->check_local == 0) {
			handler_ctx *hctx;
			char *pathinfo;

			hctx = handler_ctx_init();

			hctx->remote_conn      = con;
			hctx->plugin_data      = p;
			hctx->proc	       = NULL;
			hctx->ext              = extension;


			hctx->conf.exts        = p->conf.exts;
			hctx->conf.debug       = p->conf.debug;

			con->plugin_ctx[p->id] = hctx;

			con->mode = p->id;

			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "s",
				"handling it in mod_fastcgi");
			}

			/* the prefix is the SCRIPT_NAME,
			 * everthing from start to the next slash
			 * this is important for check-local = "disable"
			 *
			 * if prefix = /admin.fcgi
			 *
			 * /admin.fcgi/foo/bar
			 *
			 * SCRIPT_NAME = /admin.fcgi
			 * PATH_INFO   = /foo/bar
			 *
			 * if prefix = /fcgi-bin/
			 *
			 * /fcgi-bin/foo/bar
			 *
			 * SCRIPT_NAME = /fcgi-bin/foo
			 * PATH_INFO   = /bar
			 *
			 */

			/* the rewrite is only done for /prefix/? matches */
			if (extension->key->ptr[0] == '/' &&
			    con->uri.path->used > extension->key->used &&
			    NULL != (pathinfo = strchr(con->uri.path->ptr + extension->key->used - 1, '/'))) {
				/* rewrite uri.path and pathinfo */

				buffer_copy_string(con->request.pathinfo, pathinfo);

				con->uri.path->used -= con->request.pathinfo->used - 1;
				con->uri.path->ptr[con->uri.path->used - 1] = '\0';
			}
		}
	} else {
		handler_ctx *hctx;
		hctx = handler_ctx_init();

		hctx->remote_conn      = con;
		hctx->plugin_data      = p;
		hctx->proc             = NULL;
		hctx->ext              = extension;

		hctx->conf.exts        = p->conf.exts;
		hctx->conf.debug       = p->conf.debug;

		con->plugin_ctx[p->id] = hctx;

		con->mode = p->id;

		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "s", "handling it in mod_fastcgi");
		}
	}

	return HANDLER_GO_ON;
}

/* uri-path handler */
static handler_t fcgi_check_extension_1(server *srv, connection *con, void *p_d) {
	return fcgi_check_extension(srv, con, p_d, 1);
}

/* start request handler */
static handler_t fcgi_check_extension_2(server *srv, connection *con, void *p_d) {
	return fcgi_check_extension(srv, con, p_d, 0);
}

JOBLIST_FUNC(mod_fastcgi_handle_joblist) {
	plugin_data *p = p_d;
	handler_ctx *hctx = con->plugin_ctx[p->id];

	if (hctx == NULL) return HANDLER_GO_ON;

	if (hctx->sock->fd != -1) {
		switch (hctx->state) {
		case FCGI_STATE_READ:
			fdevent_event_add(srv->ev, hctx->sock, FDEVENT_IN);

			break;
		case FCGI_STATE_CONNECT_DELAYED:
		case FCGI_STATE_WRITE:
			fdevent_event_add(srv->ev, hctx->sock, FDEVENT_OUT);

			break;
		case FCGI_STATE_INIT:
			/* at reconnect */
			break;
		default:
			log_error_write(srv, __FILE__, __LINE__, "sd", "unhandled fcgi.state", hctx->state);
			break;
		}
	}

	return HANDLER_GO_ON;
}


static handler_t fcgi_connection_close_callback(server *srv, connection *con, void *p_d) {
	plugin_data *p = p_d;

	fcgi_connection_close(srv, con->plugin_ctx[p->id]);

	return HANDLER_GO_ON;
}

TRIGGER_FUNC(mod_fastcgi_handle_trigger) {
	plugin_data *p = p_d;
	size_t i, j, n;


	/* perhaps we should kill a connect attempt after 10-15 seconds
	 *
	 * currently we wait for the TCP timeout which is on Linux 180 seconds
	 *
	 */

	for (i = 0; i < srv->conns->used; i++) {
		connection *con = srv->conns->ptr[i];
		handler_ctx *hctx = con->plugin_ctx[p->id];

		/* if a connection is ours and is in handle-req for more than max-request-time
		 * kill the connection */

		if (con->mode != p->id) continue;
		if (srv->cur_ts < con->request_start + 60) continue;

		/* the request is waiting for a FCGI_STDOUT since 60 seconds */

		/* kill the connection */

		log_error_write(srv, __FILE__, __LINE__, "s", "fastcgi backend didn't responded after 60 seconds");

		fcgi_connection_close(srv, hctx);

		con->mode = DIRECT;
		con->http_status = 500;

		joblist_append(srv, con);
	}

	/* check all childs if they are still up */

	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *conf;
		fcgi_exts *exts;

		conf = p->config_storage[i];

		exts = conf->exts;

		for (j = 0; j < exts->used; j++) {
			fcgi_extension *ex;

			ex = exts->exts[j];

			for (n = 0; n < ex->used; n++) {

				fcgi_proc *proc;
				unsigned long sum_load = 0;
				fcgi_extension_host *host;

				host = ex->hosts[n];

				fcgi_restart_dead_procs(srv, p, host);

				for (proc = host->first; proc; proc = proc->next) {
					sum_load += proc->load;
				}

				if (host->num_procs &&
				    host->num_procs < host->max_procs &&
				    (sum_load / host->num_procs) > host->max_load_per_proc) {
					/* overload, spawn new child */
					if (p->conf.debug) {
						log_error_write(srv, __FILE__, __LINE__, "s",
								"overload detected, spawning a new child");
					}

					for (proc = host->unused_procs; proc && proc->pid != 0; proc = proc->next);

					if (proc) {
						if (proc == host->unused_procs) host->unused_procs = proc->next;

						if (proc->next) proc->next->prev = NULL;

						host->max_id++;
					} else {
						proc = fastcgi_process_init();
						proc->id = host->max_id++;
					}

					host->num_procs++;

					if (buffer_is_empty(host->unixsocket)) {
						proc->port = host->port + proc->id;
					} else {
						buffer_copy_string_buffer(proc->unixsocket, host->unixsocket);
						buffer_append_string(proc->unixsocket, "-");
						buffer_append_long(proc->unixsocket, proc->id);
					}

					if (fcgi_spawn_connection(srv, p, host, proc)) {
						log_error_write(srv, __FILE__, __LINE__, "s",
								"ERROR: spawning fcgi failed.");
						return HANDLER_ERROR;
					}

					proc->prev = NULL;
					proc->next = host->first;
					if (host->first) {
						host->first->prev = proc;
					}
					host->first = proc;
				}

				for (proc = host->first; proc; proc = proc->next) {
					if (proc->load != 0) break;
					if (host->num_procs <= host->min_procs) break;
					if (proc->pid == 0) continue;

					if (srv->cur_ts - proc->last_used > host->idle_timeout) {
						/* a proc is idling for a long time now,
						 * terminated it */

						if (p->conf.debug) {
							log_error_write(srv, __FILE__, __LINE__, "ssbsd",
									"idle-timeout reached, terminating child:",
									"socket:", proc->connection_name,
									"pid", proc->pid);
						}


						if (proc->next) proc->next->prev = proc->prev;
						if (proc->prev) proc->prev->next = proc->next;

						if (proc->prev == NULL) host->first = proc->next;

						proc->prev = NULL;
						proc->next = host->unused_procs;

						if (host->unused_procs) host->unused_procs->prev = proc;
						host->unused_procs = proc;

						kill(proc->pid, SIGTERM);

						proc->state = PROC_STATE_KILLED;

						log_error_write(srv, __FILE__, __LINE__, "ssbsd",
									"killed:",
									"socket:", proc->connection_name,
									"pid", proc->pid);

						host->num_procs--;

						/* proc is now in unused, let the next second handle the next process */
						break;
					}
				}

				for (proc = host->unused_procs; proc; proc = proc->next) {
					int status;

					if (proc->pid == 0) continue;
#ifndef _WIN32
					switch (waitpid(proc->pid, &status, WNOHANG)) {
					case 0:
						/* child still running after timeout, good */
						break;
					case -1:
						if (errno != EINTR) {
							/* no PID found ? should never happen */
							log_error_write(srv, __FILE__, __LINE__, "sddss",
									"pid ", proc->pid, proc->state,
									"not found:", strerror(errno));

#if 0
							if (errno == ECHILD) {
								/* someone else has cleaned up for us */
								proc->pid = 0;
								proc->state = PROC_STATE_UNSET;
							}
#endif
						}
						break;
					default:
						/* the child should not terminate at all */
						if (WIFEXITED(status)) {
							if (proc->state != PROC_STATE_KILLED) {
								log_error_write(srv, __FILE__, __LINE__, "sdb",
										"child exited:",
										WEXITSTATUS(status), proc->connection_name);
							}
						} else if (WIFSIGNALED(status)) {
							if (WTERMSIG(status) != SIGTERM) {
								log_error_write(srv, __FILE__, __LINE__, "sd",
										"child signaled:",
										WTERMSIG(status));
							}
						} else {
							log_error_write(srv, __FILE__, __LINE__, "sd",
									"child died somehow:",
									status);
						}
						proc->pid = 0;
						proc->state = PROC_STATE_UNSET;
						host->max_id--;
					}
#endif
				}
			}
		}
	}

	return HANDLER_GO_ON;
}


int mod_fastcgi_plugin_init(plugin *p) {
	p->version      = LIGHTTPD_VERSION_ID;
	p->name         = buffer_init_string("fastcgi");

	p->init         = mod_fastcgi_init;
	p->cleanup      = mod_fastcgi_free;
	p->set_defaults = mod_fastcgi_set_defaults;
	p->connection_reset        = fcgi_connection_reset;
	p->handle_connection_close = fcgi_connection_close_callback;
	p->handle_uri_clean        = fcgi_check_extension_1;
	p->handle_start_backend    = fcgi_check_extension_2;
	p->handle_send_request_content = mod_fastcgi_handle_subrequest;
	p->handle_joblist          = mod_fastcgi_handle_joblist;
	p->handle_trigger          = mod_fastcgi_handle_trigger;

	p->data         = NULL;

	return 0;
}
