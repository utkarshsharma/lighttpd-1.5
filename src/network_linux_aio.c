#define _GNU_SOURCE /* we need O_DIRECT */

#include "network_backends.h"

#ifdef USE_LINUX_AIO_SENDFILE
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
#include <fcntl.h>
#include <assert.h>

#include <libaio.h>

#include "network.h"
#include "fdevent.h"
#include "log.h"
#include "stat_cache.h"
#include "joblist.h"

#include "sys-files.h"

NETWORK_BACKEND_WRITE(linuxaiosendfile) {
	chunk *c, *tc;
	size_t chunks_written = 0;

	for(c = cq->first; c; c = c->next, chunks_written++) {
		int chunk_finished = 0;
		network_status_t ret;

		switch(c->type) {
		case MEM_CHUNK:
			ret = network_write_chunkqueue_writev_mem(srv, con, sock, cq, c);

			/* check which chunks are finished now */
			for (tc = c; tc; tc = tc->next) {
				/* finished the chunk */
				if (tc->offset == tc->mem->used - 1) {
					/* skip the first c->next as that will be done by the c = c->next in the other for()-loop */
					if (chunk_finished) {
						c = c->next;
					} else {
						chunk_finished = 1;
					}
				} else {
					break;
				}
			}

			if (ret != NETWORK_STATUS_SUCCESS) {
				return ret;
			}

			break;
		case FILE_CHUNK: {
			ssize_t r;
			int rounds = 8; 

			/* open file if not already opened */
			if (-1 == c->file.fd) {
				if (-1 == (c->file.fd = open(c->file.name->ptr, O_RDONLY | O_DIRECT | (srv->srvconf.use_noatime ? O_NOATIME : 0)))) {
					ERROR("opening '%s' failed: %s", BUF_STR(c->file.name), strerror(errno));

					return NETWORK_STATUS_FATAL_ERROR;
				}

				srv->cur_fds++;
			
#ifdef FD_CLOEXEC
				fcntl(c->file.fd, F_SETFD, FD_CLOEXEC);
#endif
			}

			do {
				size_t toSend;
				const size_t max_toSend = 2 * 256 * 1024; /** should be larger than the send buffer */
				int file_fd;
				off_t offset;
			
				offset = c->file.start + c->offset;

				toSend = c->file.length - c->offset > max_toSend ?
					max_toSend : c->file.length - c->offset;

				if (-1 == c->file.copy.fd || 0 == c->file.copy.length) {
					long page_size = sysconf(_SC_PAGESIZE);

					int res;
					int async_error = 0;

					size_t iocb_ndx;
					struct iocb *iocb = NULL;

					c->file.copy.offset = 0; 
					c->file.copy.length = toSend - (toSend % page_size); /* align to page-size */

					if (c->file.copy.length == 0) {
						async_error = 1;
					}

					/* if we reused the previous tmp-file we get overlaps 
					 * 
					 *    1 ... 3904 are ok
					 * 3905 ... 4096 are replaces by 8001 ... 8192 
					 *
					 * somehow the second read writes into the mmap() before 
					 * the sendfile is finished which is very strange.
					 *
					 * if someone finds the reason for this, feel free to remove
					 * this if again and number of reduce the syscalls a bit.
					 */
					if (-1 != c->file.copy.fd) {
						munmap(c->file.mmap.start, c->file.mmap.length);

						close(c->file.copy.fd);
						c->file.copy.fd = -1;
					}

					/* do we have a IOCB we can use ? */

					for (iocb_ndx = 0; async_error == 0 && iocb_ndx < LINUX_IO_MAX_IOCBS; iocb_ndx++) {
						if (NULL == srv->linux_io_iocbs[iocb_ndx].data) {
							iocb = &srv->linux_io_iocbs[iocb_ndx];
							break;
						}
					}

					if (iocb_ndx == LINUX_IO_MAX_IOCBS) {
						async_error = 1;
					}


					/* get mmap()ed mem-block in /dev/shm */
					if (async_error == 0 && -1 == c->file.copy.fd ) {
						char tmpfile_name[sizeof("/dev/shm/l-XXXXXX")];

						/* open a file in /dev/shm to write to */
						strcpy(tmpfile_name, "/dev/shm/l-XXXXXX");
						if (-1 == (c->file.copy.fd = mkstemp(tmpfile_name))) {
							async_error = 1;
					
							if (errno != EMFILE) {	
								TRACE("mkstemp returned: %d (%s), open fds: %d, falling back to sync-io", 
									errno, strerror(errno), srv->cur_fds);
							}
						}

						c->file.mmap.offset = 0;
						c->file.mmap.length = c->file.copy.length; /* align to page-size */

						if (!async_error) {
							srv->cur_fds++;
							unlink(tmpfile_name); /* remove the file again, we still keep it open */
							if (0 != ftruncate(c->file.copy.fd, c->file.mmap.length)) {
								/* disk full ... */
								async_error = 1;
								
								TRACE("ftruncate returned: %d (%s), open fds: %d, falling back to sync-io", 
									errno, strerror(errno), srv->cur_fds);
							}
						}

						if (!async_error) {

							c->file.mmap.start = mmap(0, c->file.mmap.length, 
									PROT_READ | PROT_WRITE, MAP_SHARED, c->file.copy.fd, 0);
							if (c->file.mmap.start == MAP_FAILED) {
								async_error = 1;
							}
						}
					}

					/* can we be sure that offset is always aligned ? */
        				if (async_error == 0 &&
					    (((intptr_t)c->file.mmap.start) % page_size != 0 ||
					     c->file.copy.length % page_size != 0 ||
					     ((intptr_t)(c->file.start + c->offset)) % page_size != 0)) {
						/** 
						 * after a fallback to sendfile() the offset might be unaligned
						 */

						async_error = 1;
					}


					/* looks like we couldn't get a temp-file [disk-full] */	
					if (async_error == 0 && -1 != c->file.copy.fd) {
		        			struct iocb *iocbs[] = { iocb };

						assert(c->file.copy.length > 0);

						io_prep_pread(iocb, c->file.fd, c->file.mmap.start, c->file.copy.length, c->file.start + c->offset);
						iocb->data = con;

					       	if (1 == (res = io_submit(srv->linux_io_ctx, 1, iocbs))) {
							srv->linux_io_waiting++;

							return NETWORK_STATUS_WAIT_FOR_AIO_EVENT;
						} else {
							iocb->data = NULL;

							if (-res != EAGAIN) {
								TRACE("io_submit returned: %d (%s), waiting jobs: %d, falling back to sync-io", 
									res, strerror(-res), srv->linux_io_waiting);
							} else {
								TRACE("io_submit returned EAGAIN on (%d - %d), -> sync-io", c->file.fd, c->file.copy.fd);
							}
						}
					}

					/* oops, looks like the IO-queue is full
					 *
					 * fall-back to blocking sendfile()
					 */

					if (c->file.mmap.start != MAP_FAILED) {
						munmap(c->file.mmap.start, c->file.mmap.length);
						c->file.mmap.start = MAP_FAILED;
					}
					if (c->file.copy.fd != -1) {
						close(c->file.copy.fd);
						srv->cur_fds--;
						c->file.copy.fd = -1;
					}

					c->file.copy.length = 0;
					c->file.copy.offset = 0;
				} else if (c->file.copy.offset == 0) {
					/* we are finished */
				}

				if (c->file.copy.fd == -1) {
					file_fd = c->file.fd;
				} else {
					file_fd = c->file.copy.fd;

					offset = c->file.copy.offset;
					toSend = c->file.copy.length - offset;
				}

				/* send the tmp-file from /dev/shm/ */
				if (-1 == (r = sendfile(sock->fd, file_fd, &offset, toSend))) {
					switch (errno) {
					case EAGAIN:
					case EINTR:
						return NETWORK_STATUS_WAIT_FOR_EVENT;
					case EPIPE:
					case ECONNRESET:
						return NETWORK_STATUS_CONNECTION_CLOSE;
					default:
						log_error_write(srv, __FILE__, __LINE__, "ssd",
								"sendfile failed:", strerror(errno), sock->fd);
						return NETWORK_STATUS_FATAL_ERROR;
					}
				}

				if (r == 0) {
					/* ... ooops */
					return NETWORK_STATUS_CONNECTION_CLOSE;
				}

				c->offset += r; /* global offset in the file */
				cq->bytes_out += r;

				if (c->file.copy.fd == -1) {
					/* ... */
				} else {
					c->file.copy.offset += r; /* local offset in the mmap file */

					if (c->file.copy.offset == c->file.copy.length) {
						c->file.copy.length = 0; /* reset the length and let the next round fetch a new item */
					}
				}

				if (c->offset == c->file.length) {
					chunk_finished = 1;
	
					if (c->file.copy.fd != -1) {
						close(c->file.copy.fd);
						c->file.copy.fd = -1;
						srv->cur_fds--;
					}

					if (c->file.fd != -1) {
						close(c->file.fd);
						c->file.fd = -1;
						srv->cur_fds--;
					}
				}

				/* the chunk is larger and the current snippet is finished */
			} while (c->file.copy.length == 0 && chunk_finished == 0 && rounds-- > 0);

			break;
		}
		default:

			log_error_write(srv, __FILE__, __LINE__, "ds", c, "type not known");

			return NETWORK_STATUS_FATAL_ERROR;
		}

		if (!chunk_finished) {
			/* not finished yet */

			return NETWORK_STATUS_WAIT_FOR_EVENT;
		}
	}

	return NETWORK_STATUS_SUCCESS;
}

#endif
#if 0
network_linuxsendfile_init(void) {
	p->write = network_linuxsendfile_write_chunkset;
}
#endif
