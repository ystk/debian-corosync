/*
 * vi: set autoindent tabstop=4 shiftwidth=4 :
 *
 * Copyright (c) 2002-2006 MontaVista Software, Inc.
 * Copyright (c) 2006-2009 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/un.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <assert.h>
#include <sys/shm.h>
#include <sys/mman.h>

#include <corosync/corotypes.h>
#include <corosync/coroipc_types.h>
#include <corosync/coroipc_ipc.h>
#include <corosync/coroipcc.h>
#include <corosync/hdb.h>

#if _POSIX_THREAD_PROCESS_SHARED > 0
#include <semaphore.h>
#else
#include <sys/sem.h>
#endif

#include "util.h"

/*
 * Define sem_wait timeout (real timeout will be (n-1;n) )
 */
#define IPC_SEMWAIT_TIMEOUT 2

struct ipc_instance {
	int fd;
#if _POSIX_THREAD_PROCESS_SHARED < 1
	int semid;
#endif
	int flow_control_state;
	struct control_buffer *control_buffer;
	char *request_buffer;
	char *response_buffer;
	char *dispatch_buffer;
	size_t control_size;
	size_t request_size;
	size_t response_size;
	size_t dispatch_size;
	uid_t euid;
	pthread_mutex_t mutex;
};

void ipc_hdb_destructor (void *context);

DECLARE_HDB_DATABASE(ipc_hdb,ipc_hdb_destructor);

#if defined(COROSYNC_LINUX) || defined(COROSYNC_SOLARIS)
#define COROSYNC_SUN_LEN(a) sizeof(*(a))
#else
#define COROSYNC_SUN_LEN(a) SUN_LEN(a)
#endif

#ifdef SO_NOSIGPIPE
static void socket_nosigpipe(int s)
{
	int on = 1;
	setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (void *)&on, sizeof(on));
}
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static cs_error_t
socket_send (
	int s,
	void *msg,
	size_t len)
{
	cs_error_t res = CS_OK;
	int result;
	struct msghdr msg_send;
	struct iovec iov_send;
	char *rbuf = msg;
	int processed = 0;

	msg_send.msg_iov = &iov_send;
	msg_send.msg_iovlen = 1;
	msg_send.msg_name = 0;
	msg_send.msg_namelen = 0;

#if !defined(COROSYNC_SOLARIS)
	msg_send.msg_control = 0;
	msg_send.msg_controllen = 0;
	msg_send.msg_flags = 0;
#else
	msg_send.msg_accrights = NULL;
	msg_send.msg_accrightslen = 0;
#endif

retry_send:
	iov_send.iov_base = &rbuf[processed];
	iov_send.iov_len = len - processed;

	result = sendmsg (s, &msg_send, MSG_NOSIGNAL);
	if (result == -1) {
		switch (errno) {
		case EINTR:
			res = CS_ERR_TRY_AGAIN;
			goto res_exit;
		case EAGAIN:
			goto retry_send;
			break;
		default:
			res = CS_ERR_LIBRARY;
			goto res_exit;
		}
	}

	processed += result;
	if (processed != len) {
		goto retry_send;
	}

	return (CS_OK);

res_exit:
	return (res);
}

static cs_error_t
socket_recv (
	int s,
	void *msg,
	size_t len)
{
	cs_error_t res = CS_OK;
	int result;
	struct msghdr msg_recv;
	struct iovec iov_recv;
	char *rbuf = msg;
	int processed = 0;

	msg_recv.msg_iov = &iov_recv;
	msg_recv.msg_iovlen = 1;
	msg_recv.msg_name = 0;
	msg_recv.msg_namelen = 0;
#if !defined (COROSYNC_SOLARIS)
	msg_recv.msg_control = 0;
	msg_recv.msg_controllen = 0;
	msg_recv.msg_flags = 0;
#else
	msg_recv.msg_accrights = NULL;
	msg_recv.msg_accrightslen = 0;
#endif


retry_recv:
	iov_recv.iov_base = (void *)&rbuf[processed];
	iov_recv.iov_len = len - processed;

	result = recvmsg (s, &msg_recv, MSG_NOSIGNAL|MSG_WAITALL);
	if (result == -1) {
		switch (errno) {
		case EINTR:
			res = CS_ERR_TRY_AGAIN;
			goto res_exit;
		case EAGAIN:
			goto retry_recv;
			break;
		default:
			res = CS_ERR_LIBRARY;
			goto res_exit;
		}
	}
#if defined(COROSYNC_SOLARIS) || defined(COROSYNC_BSD) || defined(COROSYNC_DARWIN)
	/* On many OS poll never return POLLHUP or POLLERR.
	 * EOF is detected when recvmsg return 0.
	 */
	if (result == 0) {
		res = CS_ERR_LIBRARY;
		goto res_exit;
	}
#endif

	processed += result;
	if (processed != len) {
		goto retry_recv;
	}
	assert (processed == len);
res_exit:
	return (res);
}

#if _POSIX_THREAD_PROCESS_SHARED < 1
static int
priv_change_send (struct ipc_instance *ipc_instance)
{
	char buf_req;
	mar_req_priv_change req_priv_change;
	unsigned int res;

	req_priv_change.euid = geteuid();
	/*
	 * Don't resend request unless euid has changed
	*/
	if (ipc_instance->euid == req_priv_change.euid) {
		return (0);
	}
	req_priv_change.egid = getegid();

	buf_req = MESSAGE_REQ_CHANGE_EUID;
	res = socket_send (ipc_instance->fd, &buf_req, 1);
	if (res == -1) {
		return (-1);
	}

	res = socket_send (ipc_instance->fd, &req_priv_change,
		sizeof (req_priv_change));
	if (res == -1) {
		return (-1);
	}

	ipc_instance->euid = req_priv_change.euid;
	return (0);
}

#if defined(_SEM_SEMUN_UNDEFINED)
union semun {
        int val;
        struct semid_ds *buf;
        unsigned short int *array;
        struct seminfo *__buf;
};
#endif
#endif

static int
circular_memory_map (char *path, const char *file, void **buf, size_t bytes)
{
	int fd;
	void *addr_orig;
	void *addr;
	int res;

	sprintf (path, "/dev/shm/%s", file);

	fd = mkstemp (path);
	if (fd == -1) {
		sprintf (path, LOCALSTATEDIR "/run/%s", file);
		fd = mkstemp (path);
		if (fd == -1) {
			return (-1);
		}
	}

	res = ftruncate (fd, bytes);

	addr_orig = mmap (NULL, bytes << 1, PROT_NONE,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	if (addr_orig == MAP_FAILED) {
		return (-1);
	}

	addr = mmap (addr_orig, bytes, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, fd, 0);

	if (addr != addr_orig) {
		return (-1);
	}
#ifdef COROSYNC_BSD
	madvise(addr_orig, bytes, MADV_NOSYNC);
#endif

	addr = mmap (((char *)addr_orig) + bytes,
                  bytes, PROT_READ | PROT_WRITE,
                  MAP_FIXED | MAP_SHARED, fd, 0);
#ifdef COROSYNC_BSD
	madvise(((char *)addr_orig) + bytes, bytes, MADV_NOSYNC);
#endif

	res = close (fd);
	if (res) {
		return (-1);
	}
	*buf = addr_orig;
	return (0);
}

static void
memory_unmap (void *addr, size_t bytes)
{
	int res;

	res = munmap (addr, bytes);
}

void ipc_hdb_destructor (void *context ) {
	struct ipc_instance *ipc_instance = (struct ipc_instance *)context;

	/*
	 * << 1 (or multiplied by 2) because this is a wrapped memory buffer
	 */
	memory_unmap (ipc_instance->control_buffer, ipc_instance->control_size);
	memory_unmap (ipc_instance->request_buffer, ipc_instance->request_size);
	memory_unmap (ipc_instance->response_buffer, ipc_instance->response_size);
	memory_unmap (ipc_instance->dispatch_buffer, (ipc_instance->dispatch_size) << 1);
}
static int
memory_map (char *path, const char *file, void **buf, size_t bytes)
{
	int fd;
	void *addr_orig;
	void *addr;
	int res;

	sprintf (path, "/dev/shm/%s", file);

	fd = mkstemp (path);
	if (fd == -1) {
		sprintf (path, LOCALSTATEDIR "/run/%s", file);
		fd = mkstemp (path);
		if (fd == -1) {
			return (-1);
		}
	}

	res = ftruncate (fd, bytes);

	addr_orig = mmap (NULL, bytes, PROT_NONE,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);

	if (addr_orig == MAP_FAILED) {
		return (-1);
	}

	addr = mmap (addr_orig, bytes, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_SHARED, fd, 0);

	if (addr != addr_orig) {
		return (-1);
	}
#ifdef COROSYNC_BSD
	madvise(addr_orig, bytes, MADV_NOSYNC);
#endif

	res = close (fd);
	if (res) {
		return (-1);
	}
	*buf = addr_orig;
	return (0);
}

static cs_error_t
msg_send (
	struct ipc_instance *ipc_instance,
	const struct iovec *iov,
	unsigned int iov_len)
{
#if _POSIX_THREAD_PROCESS_SHARED < 1
	struct sembuf sop;
#endif

	int i;
	int res;
	int req_buffer_idx = 0;

	for (i = 0; i < iov_len; i++) {
		if ((req_buffer_idx + iov[i].iov_len) > 
			ipc_instance->request_size) {
			return (CS_ERR_INVALID_PARAM);
		}
		memcpy (&ipc_instance->request_buffer[req_buffer_idx],
			iov[i].iov_base,
			iov[i].iov_len);
		req_buffer_idx += iov[i].iov_len;
	}

#if _POSIX_THREAD_PROCESS_SHARED > 0
	res = sem_post (&ipc_instance->control_buffer->sem0);
	if (res == -1) {
		return (CS_ERR_LIBRARY);
	}
#else 
	/*
	 * Signal semaphore #0 indicting a new message from client
	 * to server request queue
	 */
	sop.sem_num = 0;
	sop.sem_op = 1;
	sop.sem_flg = 0;

retry_semop:
	res = semop (ipc_instance->semid, &sop, 1);
	if (res == -1 && errno == EINTR) {
		return (CS_ERR_TRY_AGAIN);
	} else
	if (res == -1 && errno == EACCES) {
		priv_change_send (ipc_instance);
		goto retry_semop;
	} else
	if (res == -1) {
		return (CS_ERR_LIBRARY);
	}
#endif
	return (CS_OK);
}

static cs_error_t
reply_receive (
	struct ipc_instance *ipc_instance,
	void *res_msg,
	size_t res_len)
{
#if _POSIX_THREAD_PROCESS_SHARED < 1
	struct sembuf sop;
#else
	struct timespec timeout;
	struct pollfd pfd;
#endif
	coroipc_response_header_t *response_header;
	int res;

#if _POSIX_THREAD_PROCESS_SHARED > 0
retry_semwait:
	timeout.tv_sec = time(NULL) + IPC_SEMWAIT_TIMEOUT;
	timeout.tv_nsec = 0;

	res = sem_timedwait (&ipc_instance->control_buffer->sem1, &timeout);
	if (res == -1 && errno == ETIMEDOUT) {
		pfd.fd = ipc_instance->fd;
		pfd.events = 0;

		poll (&pfd, 1, 0);
		if (pfd.revents == POLLERR || pfd.revents == POLLHUP) {
			return (CS_ERR_LIBRARY);
		}

		goto retry_semwait;
	}

	if (res == -1 && errno == EINTR) {
		goto retry_semwait;
	}
#else
	/*
	 * Wait for semaphore #1 indicating a new message from server
	 * to client in the response queue
	 */
	sop.sem_num = 1;
	sop.sem_op = -1;
	sop.sem_flg = 0;

retry_semop:
	res = semop (ipc_instance->semid, &sop, 1);
	if (res == -1 && errno == EINTR) {
		return (CS_ERR_TRY_AGAIN);
	} else
	if (res == -1 && errno == EACCES) {
		priv_change_send (ipc_instance);
		goto retry_semop;
	} else
	if (res == -1) {
		return (CS_ERR_LIBRARY);
	}
#endif

	response_header = (coroipc_response_header_t *)ipc_instance->response_buffer;
	if (response_header->error == CS_ERR_TRY_AGAIN) {
		return (CS_ERR_TRY_AGAIN);
	}

	memcpy (res_msg, ipc_instance->response_buffer, res_len);
	return (CS_OK);
}

static cs_error_t
reply_receive_in_buf (
	struct ipc_instance *ipc_instance,
	void **res_msg)
{
#if _POSIX_THREAD_PROCESS_SHARED < 1
	struct sembuf sop;
#else
	struct timespec timeout;
	struct pollfd pfd;
#endif
	int res;

#if _POSIX_THREAD_PROCESS_SHARED > 0
retry_semwait:
	timeout.tv_sec = time(NULL) + IPC_SEMWAIT_TIMEOUT;
	timeout.tv_nsec = 0;

	res = sem_timedwait (&ipc_instance->control_buffer->sem1, &timeout);
	if (res == -1 && errno == ETIMEDOUT) {
		pfd.fd = ipc_instance->fd;
		pfd.events = 0;

		poll (&pfd, 1, 0);
		if (pfd.revents == POLLERR || pfd.revents == POLLHUP) {
			return (CS_ERR_LIBRARY);
		}

		goto retry_semwait;
	}

	if (res == -1 && errno == EINTR) {
		goto retry_semwait;
	}
#else
	/*
	 * Wait for semaphore #1 indicating a new message from server
	 * to client in the response queue
	 */
	sop.sem_num = 1;
	sop.sem_op = -1;
	sop.sem_flg = 0;

retry_semop:
	res = semop (ipc_instance->semid, &sop, 1);
	if (res == -1 && errno == EINTR) {
		return (CS_ERR_TRY_AGAIN);
	} else
	if (res == -1 && errno == EACCES) {
		priv_change_send (ipc_instance);
		goto retry_semop;
	} else
	if (res == -1) {
		return (CS_ERR_LIBRARY);
	}
#endif

	*res_msg = (char *)ipc_instance->response_buffer;
	return (CS_OK);
}

/*
 * External API
 */
cs_error_t
coroipcc_service_connect (
	const char *socket_name,
	unsigned int service,
	size_t request_size,
	size_t response_size,
	size_t dispatch_size,
	hdb_handle_t *handle)

{
	int request_fd;
	struct sockaddr_un address;
	cs_error_t res;
	struct ipc_instance *ipc_instance;
#if _POSIX_THREAD_PROCESS_SHARED < 1
	key_t semkey = 0;
	union semun semun;
#endif
	int sys_res;
	mar_req_setup_t req_setup;
	mar_res_setup_t res_setup;
	char control_map_path[128];
	char request_map_path[128];
	char response_map_path[128];
	char dispatch_map_path[128];

	res = hdb_error_to_cs (hdb_handle_create (&ipc_hdb,
		sizeof (struct ipc_instance), handle));
	if (res != CS_OK) {
		return (res);
	}

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, *handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}

	res_setup.error = CS_ERR_LIBRARY;

#if defined(COROSYNC_SOLARIS)
	request_fd = socket (PF_UNIX, SOCK_STREAM, 0);
#else
	request_fd = socket (PF_LOCAL, SOCK_STREAM, 0);
#endif
	if (request_fd == -1) {
		return (CS_ERR_LIBRARY);
	}
#ifdef SO_NOSIGPIPE
	socket_nosigpipe (request_fd);
#endif

	memset (&address, 0, sizeof (struct sockaddr_un));
	address.sun_family = AF_UNIX;
#if defined(COROSYNC_BSD) || defined(COROSYNC_DARWIN)
	address.sun_len = SUN_LEN(&address);
#endif

#if defined(COROSYNC_LINUX)
	sprintf (address.sun_path + 1, "%s", socket_name);
#else
	sprintf (address.sun_path, "%s/%s", SOCKETDIR, socket_name);
#endif
	sys_res = connect (request_fd, (struct sockaddr *)&address,
		COROSYNC_SUN_LEN(&address));
	if (sys_res == -1) {
		res = CS_ERR_TRY_AGAIN;
		goto error_connect;
	}

	res = memory_map (
		control_map_path,
		"control_buffer-XXXXXX",
		(void *)&ipc_instance->control_buffer,
		8192);
	if (res == -1) {
		res = CS_ERR_LIBRARY;
		goto error_connect;
	}

	res = memory_map (
		request_map_path,
		"request_buffer-XXXXXX",
		(void *)&ipc_instance->request_buffer,
		request_size);
	if (res == -1) {
		res = CS_ERR_LIBRARY;
		goto error_request_buffer;
	}

	res = memory_map (
		response_map_path,
		"response_buffer-XXXXXX",
		(void *)&ipc_instance->response_buffer,
		response_size);
	if (res == -1) {
		res = CS_ERR_LIBRARY;
		goto error_response_buffer;
	}

	res = circular_memory_map (
		dispatch_map_path,
		"dispatch_buffer-XXXXXX",
		(void *)&ipc_instance->dispatch_buffer,
		dispatch_size);
	if (res == -1) {
		res = CS_ERR_LIBRARY;
		goto error_dispatch_buffer;
	}

#if _POSIX_THREAD_PROCESS_SHARED > 0
	sem_init (&ipc_instance->control_buffer->sem0, 1, 0);
	sem_init (&ipc_instance->control_buffer->sem1, 1, 0);
	sem_init (&ipc_instance->control_buffer->sem2, 1, 0);
#else

	/*
	 * Allocate a semaphore segment
	 */
	while (1) {
		semkey = random();
		ipc_instance->euid = geteuid ();
		if ((ipc_instance->semid
		     = semget (semkey, 3, IPC_CREAT|IPC_EXCL|0600)) != -1) {
		      break;
		}
		/*
		 * EACCESS can be returned as non root user when opening a different
		 * users semaphore.
		 *
		 * EEXIST can happen when we are a root or nonroot user opening
		 * an existing shared memory segment for which we have access
		 */
		if (errno != EEXIST && errno != EACCES) {
			goto error_exit;
		}
	}

	semun.val = 0;
	res = semctl (ipc_instance->semid, 0, SETVAL, semun);
	if (res != 0) {
		goto error_exit;
	}

	res = semctl (ipc_instance->semid, 1, SETVAL, semun);
	if (res != 0) {
		goto error_exit;
	}
#endif

	/*
	 * Initialize IPC setup message
	 */
	req_setup.service = service;
	strcpy (req_setup.control_file, control_map_path);
	strcpy (req_setup.request_file, request_map_path);
	strcpy (req_setup.response_file, response_map_path);
	strcpy (req_setup.dispatch_file, dispatch_map_path);
	req_setup.control_size = 8192;
	req_setup.request_size = request_size;
	req_setup.response_size = response_size;
	req_setup.dispatch_size = dispatch_size;

#if _POSIX_THREAD_PROCESS_SHARED < 1
	req_setup.semkey = semkey;
#endif

	res = socket_send (request_fd, &req_setup, sizeof (mar_req_setup_t));
	if (res != CS_OK) {
		goto error_exit;
	}
	res = socket_recv (request_fd, &res_setup, sizeof (mar_res_setup_t));
	if (res != CS_OK) {
		goto error_exit;
	}

	ipc_instance->fd = request_fd;
	ipc_instance->flow_control_state = 0;

	if (res_setup.error == CS_ERR_TRY_AGAIN) {
		res = res_setup.error;
		goto error_exit;
	}

	ipc_instance->control_size = 8192;
	ipc_instance->request_size = request_size;
	ipc_instance->response_size = response_size;
	ipc_instance->dispatch_size = dispatch_size;

	pthread_mutex_init (&ipc_instance->mutex, NULL);

	hdb_handle_put (&ipc_hdb, *handle);

	return (res_setup.error);

error_exit:
#if _POSIX_THREAD_PROCESS_SHARED < 1
	if (ipc_instance->semid > 0)
		semctl (ipc_instance->semid, 0, IPC_RMID);
#endif
	memory_unmap (ipc_instance->dispatch_buffer, dispatch_size);
error_dispatch_buffer:
	memory_unmap (ipc_instance->response_buffer, response_size);
error_response_buffer:
	memory_unmap (ipc_instance->request_buffer, request_size);
error_request_buffer:
	memory_unmap (ipc_instance->control_buffer, 8192);
error_connect:
	close (request_fd);

	hdb_handle_destroy (&ipc_hdb, *handle);
	hdb_handle_put (&ipc_hdb, *handle);

	return (res);
}

cs_error_t
coroipcc_service_disconnect (
	hdb_handle_t handle)
{
	cs_error_t res;
	struct ipc_instance *ipc_instance;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}

	shutdown (ipc_instance->fd, SHUT_RDWR);
	close (ipc_instance->fd);
	hdb_handle_destroy (&ipc_hdb, handle);
	hdb_handle_put (&ipc_hdb, handle);
	return (CS_OK);
}

cs_error_t
coroipcc_dispatch_flow_control_get (
	hdb_handle_t handle,
	unsigned int *flow_control_state)
{
	struct ipc_instance *ipc_instance;
	cs_error_t res;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}

	*flow_control_state = ipc_instance->flow_control_state;

	hdb_handle_put (&ipc_hdb, handle);
	return (res);
}

cs_error_t
coroipcc_fd_get (
	hdb_handle_t handle,
	int *fd)
{
	struct ipc_instance *ipc_instance;
	cs_error_t res;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}

	*fd = ipc_instance->fd;

	hdb_handle_put (&ipc_hdb, handle);
	return (res);
}

cs_error_t
coroipcc_dispatch_get (
	hdb_handle_t handle,
	void **data,
	int timeout)
{
	struct pollfd ufds;
	int poll_events;
	char buf;
	struct ipc_instance *ipc_instance;
	int res;
	char buf_two = 1;
	char *data_addr;
	cs_error_t error = CS_OK;

	error = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (error != CS_OK) {
		return (error);
	}

	*data = NULL;

	ufds.fd = ipc_instance->fd;
	ufds.events = POLLIN;
	ufds.revents = 0;

	poll_events = poll (&ufds, 1, timeout);
	if (poll_events == -1 && errno == EINTR) {
		error = CS_ERR_TRY_AGAIN;
		goto error_put;
	} else
	if (poll_events == -1) {
		error = CS_ERR_LIBRARY;
		goto error_put;
	} else
	if (poll_events == 0) {
		error = CS_ERR_TRY_AGAIN;
		goto error_put;
	}
	if (poll_events == 1 && (ufds.revents & (POLLERR|POLLHUP))) {
		error = CS_ERR_LIBRARY;
		goto error_put;
	}

	res = recv (ipc_instance->fd, &buf, 1, 0);
	if (res == -1 && errno == EINTR) {
		error = CS_ERR_TRY_AGAIN;
		goto error_put;
	} else
	if (res == -1) {
		error = CS_ERR_LIBRARY;
		goto error_put;
	} else
	if (res == 0) {
		/* Means that the peer closed cleanly the socket. However, it should
		 * happen only on BSD and Darwing systems since poll() returns a
		 * POLLHUP event on other systems.
		 */
		error = CS_ERR_LIBRARY;
		goto error_put;
	}
	ipc_instance->flow_control_state = 0;
	if (buf == MESSAGE_RES_OUTQ_NOT_EMPTY || buf == MESSAGE_RES_ENABLE_FLOWCONTROL) {
		ipc_instance->flow_control_state = 1;
	}
	/*
	 * Notify executive to flush any pending dispatch messages
	 */
	if (ipc_instance->flow_control_state) {
		buf_two = MESSAGE_REQ_OUTQ_FLUSH;
		res = socket_send (ipc_instance->fd, &buf_two, 1);
		assert (res == CS_OK); /* TODO */
	}
	/*
	 * This is just a notification of flow control starting at the addition
	 * of a new pending message, not a message to dispatch
	 */
	if (buf == MESSAGE_RES_ENABLE_FLOWCONTROL) {
		error = CS_ERR_TRY_AGAIN;
		goto error_put;
	}
	if (buf == MESSAGE_RES_OUTQ_FLUSH_NR) {
		error = CS_ERR_TRY_AGAIN;
		goto error_put;
	}

	data_addr = ipc_instance->dispatch_buffer;

	data_addr = &data_addr[ipc_instance->control_buffer->read];

	*data = (void *)data_addr;

	return (CS_OK);
error_put:
	hdb_handle_put (&ipc_hdb, handle);
	return (error);
}

cs_error_t
coroipcc_dispatch_put (hdb_handle_t handle)
{
#if _POSIX_THREAD_PROCESS_SHARED < 1
	struct sembuf sop;
#endif
	coroipc_response_header_t *header;
	struct ipc_instance *ipc_instance;
	int res;
	char *addr;
	unsigned int read_idx;

	res = hdb_error_to_cs (hdb_handle_get_always (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}
#if _POSIX_THREAD_PROCESS_SHARED > 0
retry_semwait:
	res = sem_wait (&ipc_instance->control_buffer->sem2);
	if (res == -1 && errno == EINTR) {
		goto retry_semwait;
	}
#else
	sop.sem_num = 2;
	sop.sem_op = -1;
	sop.sem_flg = 0;
retry_semop:
	res = semop (ipc_instance->semid, &sop, 1);
	if (res == -1 && errno == EINTR) {
		res = CS_ERR_TRY_AGAIN;
		goto error_exit;
	} else
	if (res == -1 && errno == EACCES) {
		priv_change_send (ipc_instance);
		goto retry_semop;
	} else
	if (res == -1) {
		res = CS_ERR_LIBRARY;
		goto error_exit;
	}
#endif

	addr = ipc_instance->dispatch_buffer;

	read_idx = ipc_instance->control_buffer->read;
	header = (coroipc_response_header_t *) &addr[read_idx];
	ipc_instance->control_buffer->read =
		(read_idx + header->size) % ipc_instance->dispatch_size;
	/*
	 * Put from dispatch get and also from this call's get
	 */
	res = CS_OK;
	
#if _POSIX_THREAD_PROCESS_SHARED < 1
error_exit:
#endif
	hdb_handle_put (&ipc_hdb, handle);
	hdb_handle_put (&ipc_hdb, handle);

	return (res);
}

cs_error_t
coroipcc_msg_send_reply_receive (
	hdb_handle_t handle,
	const struct iovec *iov,
	unsigned int iov_len,
	void *res_msg,
	size_t res_len)
{
	cs_error_t res;
	struct ipc_instance *ipc_instance;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}

	pthread_mutex_lock (&ipc_instance->mutex);

	res = msg_send (ipc_instance, iov, iov_len);
	if (res != CS_OK) {
		goto error_exit;
	}

	res = reply_receive (ipc_instance, res_msg, res_len);

error_exit:
	hdb_handle_put (&ipc_hdb, handle);
	pthread_mutex_unlock (&ipc_instance->mutex);

	return (res);
}

cs_error_t
coroipcc_msg_send_reply_receive_in_buf_get (
	hdb_handle_t handle,
	const struct iovec *iov,
	unsigned int iov_len,
	void **res_msg)
{
	unsigned int res;
	struct ipc_instance *ipc_instance;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}

	pthread_mutex_lock (&ipc_instance->mutex);

	res = msg_send (ipc_instance, iov, iov_len);
	if (res != CS_OK) {
		goto error_exit;
	}

	res = reply_receive_in_buf (ipc_instance, res_msg);

error_exit:
	pthread_mutex_unlock (&ipc_instance->mutex);

	return (res);
}

cs_error_t
coroipcc_msg_send_reply_receive_in_buf_put (
	hdb_handle_t handle)
{
	unsigned int res;
	struct ipc_instance *ipc_instance;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}
	hdb_handle_put (&ipc_hdb, handle);
	hdb_handle_put (&ipc_hdb, handle);

	return (res);
}

cs_error_t
coroipcc_zcb_alloc (
	hdb_handle_t handle,
	void **buffer,
	size_t size,
	size_t header_size)
{
	struct ipc_instance *ipc_instance;
	void *buf = NULL;
	char path[128];
	unsigned int res;
	mar_req_coroipcc_zc_alloc_t req_coroipcc_zc_alloc;
	coroipc_response_header_t res_coroipcs_zc_alloc;
	size_t map_size;
	struct iovec iovec;
	struct coroipcs_zc_header *hdr;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}
	map_size = size + header_size + sizeof (struct coroipcs_zc_header);
	res = memory_map (path, "corosync_zerocopy-XXXXXX", &buf, map_size);
	assert (res != -1);

	req_coroipcc_zc_alloc.header.size = sizeof (mar_req_coroipcc_zc_alloc_t);
	req_coroipcc_zc_alloc.header.id = ZC_ALLOC_HEADER;
	req_coroipcc_zc_alloc.map_size = map_size;
	strcpy (req_coroipcc_zc_alloc.path_to_file, path);


	iovec.iov_base = (void *)&req_coroipcc_zc_alloc;
	iovec.iov_len = sizeof (mar_req_coroipcc_zc_alloc_t);

	res = coroipcc_msg_send_reply_receive (
		handle,
		&iovec,
		1,
		&res_coroipcs_zc_alloc,
		sizeof (coroipc_response_header_t));

	hdr = (struct coroipcs_zc_header *)buf;
	hdr->map_size = map_size;
	*buffer = ((char *)buf) + sizeof (struct coroipcs_zc_header);

	hdb_handle_put (&ipc_hdb, handle);
	return (res);
}

cs_error_t
coroipcc_zcb_free (
	hdb_handle_t handle,
	void *buffer)
{
	struct ipc_instance *ipc_instance;
	mar_req_coroipcc_zc_free_t req_coroipcc_zc_free;
	coroipc_response_header_t res_coroipcs_zc_free;
	struct iovec iovec;
	unsigned int res;
	struct coroipcs_zc_header *header = (struct coroipcs_zc_header *)((char *)buffer - sizeof (struct coroipcs_zc_header));

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}

	req_coroipcc_zc_free.header.size = sizeof (mar_req_coroipcc_zc_free_t);
	req_coroipcc_zc_free.header.id = ZC_FREE_HEADER;
	req_coroipcc_zc_free.map_size = header->map_size;
	req_coroipcc_zc_free.server_address = header->server_address;

	iovec.iov_base = (void *)&req_coroipcc_zc_free;
	iovec.iov_len = sizeof (mar_req_coroipcc_zc_free_t);

	res = coroipcc_msg_send_reply_receive (
		handle,
		&iovec,
		1,
		&res_coroipcs_zc_free,
		sizeof (coroipc_response_header_t));

	munmap ((void *)header, header->map_size);

	hdb_handle_put (&ipc_hdb, handle);

	return (res);
}

cs_error_t
coroipcc_zcb_msg_send_reply_receive (
	hdb_handle_t handle,
        void *msg,
        void *res_msg,
        size_t res_len)
{
	struct ipc_instance *ipc_instance;
	mar_req_coroipcc_zc_execute_t req_coroipcc_zc_execute;
	struct coroipcs_zc_header *hdr;
	struct iovec iovec;
	cs_error_t res;

	res = hdb_error_to_cs (hdb_handle_get (&ipc_hdb, handle, (void **)&ipc_instance));
	if (res != CS_OK) {
		return (res);
	}
	hdr = (struct coroipcs_zc_header *)(((char *)msg) - sizeof (struct coroipcs_zc_header));

	req_coroipcc_zc_execute.header.size = sizeof (mar_req_coroipcc_zc_execute_t);
	req_coroipcc_zc_execute.header.id = ZC_EXECUTE_HEADER;
	req_coroipcc_zc_execute.server_address = hdr->server_address;

	iovec.iov_base = (void *)&req_coroipcc_zc_execute;
	iovec.iov_len = sizeof (mar_req_coroipcc_zc_execute_t);

	res = coroipcc_msg_send_reply_receive (
		handle,
		&iovec,
		1,
		res_msg,
		res_len);

	hdb_handle_put (&ipc_hdb, handle);
	return (res);
}
