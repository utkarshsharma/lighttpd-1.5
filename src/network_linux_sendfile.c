#include "network_backends.h"

#ifdef USE_LINUX_SENDFILE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <stdlib.h>

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "file_cache.h"


int network_write_chunkqueue_linuxsendfile(server *srv, connection *con, chunkqueue *cq) {
	const int fd = con->fd;
	chunk *c;
	size_t chunks_written = 0;
	
	for(c = cq->first; c; c = c->next, chunks_written++) {
		int chunk_finished = 0;
		
		switch(c->type) {
		case MEM_CHUNK: {
			char * offset;
			size_t toSend;
			ssize_t r;
			
			size_t num_chunks, i;
			struct iovec chunks[UIO_MAXIOV];
			chunk *tc;
			size_t num_bytes = 0;
			
			/* we can't send more then SSIZE_MAX bytes in one chunk */
			
			/* build writev list 
			 * 
			 * 1. limit: num_chunks < UIO_MAXIOV
			 * 2. limit: num_bytes < SSIZE_MAX
			 */
			for (num_chunks = 0, tc = c; 
			     tc && tc->type == MEM_CHUNK && num_chunks < UIO_MAXIOV; 
			     tc = tc->next, num_chunks++);
			
			for (tc = c, i = 0; i < num_chunks; tc = tc->next, i++) {
				if (tc->data.mem->used == 0) {
					chunks[i].iov_base = tc->data.mem->ptr;
					chunks[i].iov_len  = 0;
				} else {
					offset = tc->data.mem->ptr + tc->offset;
					toSend = tc->data.mem->used - 1 - tc->offset;
				
					chunks[i].iov_base = offset;
					
					/* protect the return value of writev() */
					if (toSend > SSIZE_MAX ||
					    num_bytes + toSend > SSIZE_MAX) {
						chunks[i].iov_len = SSIZE_MAX - num_bytes;
						
						num_chunks = i + 1;
						break;
					} else {
						chunks[i].iov_len = toSend;
					}
				 
					num_bytes += toSend;
				}
			}
			
			if ((r = writev(fd, chunks, num_chunks)) < 0) {
				switch (errno) {
				case EAGAIN:
				case EINTR:
					r = 0;
					break;
				case EPIPE:
				case ECONNRESET:
					return -2;
				default:
					log_error_write(srv, __FILE__, __LINE__, "ssd", 
							"writev failed:", strerror(errno), fd);
				
					return -1;
				}
			}
			
			/* check which chunks have been written */
			
			for(i = 0, tc = c; i < num_chunks; i++, tc = tc->next) {
				if (r >= (ssize_t)chunks[i].iov_len) {
					/* written */
					r -= chunks[i].iov_len;
					tc->offset += chunks[i].iov_len;
					con->bytes_written += chunks[i].iov_len;
					
					if (chunk_finished) {
						/* skip the chunks from further touches */
						chunks_written++;
						c = c->next;
					} else {
						/* chunks_written + c = c->next is done in the for()*/
						chunk_finished++;
					}
				} else {
					/* partially written */
					
					tc->offset += r;
					con->bytes_written += r;
					chunk_finished = 0;
					
					break;
				}
			}
			
			break;
		}
		case FILE_CHUNK: {
			ssize_t r;
			off_t offset;
			size_t toSend;
			
			switch(file_cache_get_entry(srv, con, c->data.file.name, &(con->fce))) {
			case HANDLER_GO_ON:
				offset = c->data.file.offset + c->offset;
				/* limit the toSend to 2^31-1 bytes in a chunk */
				toSend = c->data.file.length - c->offset > ((1 << 30) - 1) ? 
					((1 << 30) - 1) : c->data.file.length - c->offset;
				
				if (offset > con->fce->st.st_size) {
					log_error_write(srv, __FILE__, __LINE__, "sb", "file was shrinked:", c->data.file.name);
					
					return -1;
				}
				
				/* Linux sendfile() */
				if (-1 == (r = sendfile(fd, con->fce->fd, &offset, toSend))) {
					if (errno != EAGAIN && 
					    errno != EINTR) {
						log_error_write(srv, __FILE__, __LINE__, "ssd", "sendfile:", strerror(errno), errno);
						
						return -1;
					}
					
					r = 0;
				}
				
				break;
			case HANDLER_WAIT_FOR_FD:
				/* comeback later */
				
				log_error_write(srv, __FILE__, __LINE__, "ssd", "sendfile (handled):", strerror(errno), errno);
				
				r = 0;
				
				break;
			default:
				log_error_write(srv, __FILE__, __LINE__, "sb",
						strerror(errno), c->data.file.name);
				
				return -1;
			}
			
			c->offset += r;
			con->bytes_written += r;
			
			if (c->offset == c->data.file.length) {
				chunk_finished = 1;
			}
			
			break;
		}
		default:
			
			log_error_write(srv, __FILE__, __LINE__, "ds", c, "type not known");
			
			return -1;
		}
		
		if (!chunk_finished) {
			/* not finished yet */
			
			break;
		}
	}

	return chunks_written;
}

#endif
#if 0
network_linuxsendfile_init(void) {
	p->write = network_linuxsendfile_write_chunkset;
}
#endif