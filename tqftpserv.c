// SPDX-License-Identifier: BSD-3-Clause
/*
 * Copyright (c) 2018, Linaro Ltd.
 */
#include <arpa/inet.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <libqrtr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "list.h"
#include "translate.h"
#include "zstd-decompress.h"

#define MAX(x, y) ((x) > (y) ? (x) : (y))

enum {
	OP_RRQ = 1,
	OP_WRQ,
	OP_DATA,
	OP_ACK,
	OP_ERROR,
	OP_OACK,
};

enum {
	ERROR_END_OF_TRANSFER = 9,
};

struct tftp_client {
	struct list_head node;

	struct sockaddr_qrtr sq;

	int sock;
	int fd;

	size_t block;

	size_t blksize;
	size_t rsize;
	size_t wsize;
	unsigned int timeoutms;
	off_t seek;
};

static struct list_head readers = LIST_INIT(readers);
static struct list_head writers = LIST_INIT(writers);

static ssize_t tftp_send_data(struct tftp_client *client,
			      unsigned int block, size_t offset, size_t response_size)
{
	ssize_t len;
	size_t send_len;
	char *buf;
	char *p;

	buf = malloc(4 + client->blksize);
	p = buf;

	*p++ = 0;
	*p++ = OP_DATA;

	*p++ = (block >> 8) & 0xff;
	*p++ = block & 0xff;

	len = pread(client->fd, p, client->blksize, offset);
	if (len < 0) {
		printf("[TQFTP] failed to read data\n");
		free(buf);
		return len;
	}

	p += len;

	/* If rsize was set, we should limit the data in the response to n bytes */
	if (response_size != 0) {
		/* Header (4 bytes) + data size */
		send_len = 4 + response_size;
		if (send_len > p - buf) {
			printf("[TQFTP] requested data of %ld bytes but only read %ld bytes from file, rejecting\n", response_size, len);
			free(buf);
			return -EINVAL;
		}
	} else {
		send_len = p - buf;
	}

	// printf("[TQFTP] Sending %zd bytes of DATA\n", send_len);
	len = send(client->sock, buf, send_len, 0);

	free(buf);

	return len;
}


static int tftp_send_ack(int sock, int block)
{
	struct {
		uint16_t opcode;
		uint16_t block;
	} ack = { htons(OP_ACK), htons(block) };

	return send(sock, &ack, sizeof(ack), 0);
}

static int tftp_send_oack(int sock, size_t *blocksize, size_t *tsize,
			  size_t *wsize, unsigned int *timeoutms, size_t *rsize,
			  off_t *seek)
{
	char buf[512];
	char *p = buf;
	int n;

	*p++ = 0;
	*p++ = OP_OACK;

	if (blocksize) {
		strcpy(p, "blksize");
		p += 8;

		n = sprintf(p, "%zd", *blocksize);
		p += n;
		*p++ = '\0';
	}

	if (timeoutms) {
		strcpy(p, "timeoutms");
		p += 10;

		n = sprintf(p, "%d", *timeoutms);
		p += n;
		*p++ = '\0';
	}

	if (tsize && *tsize != -1) {
		strcpy(p, "tsize");
		p += 6;

		n = sprintf(p, "%zd", *tsize);
		p += n;
		*p++ = '\0';
	}

	if (wsize) {
		strcpy(p, "wsize");
		p += 6;

		n = sprintf(p, "%zd", *wsize);
		p += n;
		*p++ = '\0';
	}

	if (rsize) {
		strcpy(p, "rsize");
		p += 6;

		n = sprintf(p, "%zd", *rsize);
		p += n;
		*p++ = '\0';
	}

	if (seek) {
		strcpy(p, "seek");
		p += 5;

		n = sprintf(p, "%zd", *seek);
		p += n;
		*p++ = '\0';
	}

	return send(sock, buf, p - buf, 0);
}

static int tftp_send_error(int sock, int code, const char *msg)
{
	size_t len;
	char *buf;
	int rc;

	len = 4 + strlen(msg) + 1;

	buf = calloc(1, len);
	if (!buf)
		return -1;

	*(uint16_t*)buf = htons(OP_ERROR);
	*(uint16_t*)(buf + 2) = htons(code);
	strcpy(buf + 4, msg);

	rc = send(sock, buf, len, 0);
	free(buf);
	return rc;
}

static void parse_options(const char *buf, size_t len, size_t *blksize,
			  ssize_t *tsize, size_t *wsize, unsigned int *timeoutms,
			  size_t *rsize, off_t *seek)
{
	const char *opt, *value;
	const char *p = buf;

	while (p < buf + len) {
		/* XXX: ensure we're not running off the end */
		opt = p;
		p += strlen(p) + 1;

		/* XXX: ensure we're not running off the end */
		value = p;
		p += strlen(p) + 1;

		/*
		 * blksize: block size - how many bytes to send at once
		 * timeoutms: timeout in milliseconds
		 * tsize: total size - request to get file size in bytes
		 * rsize: read size - how many bytes to send, not full file
		 * wsize: window size - how many blocks to send without ACK
		 * seek: offset from beginning of file in bytes to start reading
		 */
		if (!strcmp(opt, "blksize")) {
			*blksize = atoi(value);
		} else if (!strcmp(opt, "timeoutms")) {
			*timeoutms = atoi(value);
		} else if (!strcmp(opt, "tsize")) {
			*tsize = atoi(value);
		} else if (!strcmp(opt, "rsize")) {
			*rsize = atoi(value);
		} else if (!strcmp(opt, "wsize")) {
			*wsize = atoi(value);
		} else if (!strcmp(opt, "seek")) {
			*seek = atoi(value);
		} else {
			printf("[TQFTP] Ignoring unknown option '%s' with value '%s'\n", opt, value);
		}
	}
}

static void handle_rrq(const char *buf, size_t len, struct sockaddr_qrtr *sq)
{
	struct tftp_client *client;
	const char *filename;
	const char *mode;
	struct stat sb;
	const char *p;
	ssize_t tsize = -1;
	size_t blksize = 512;
	unsigned int timeoutms = 1000;
	size_t rsize = 0;
	size_t wsize = 0;
	off_t seek = 0;
	bool do_oack = false;
	int sock;
	int ret;
	int fd;

	p = buf + 2;

	filename = p;
	p += strlen(p) + 1;

	mode = p;
	p += strlen(p) + 1;

	if (strcasecmp(mode, "octet")) {
		/* XXX: error */
		printf("[TQFTP] not octet, reject\n");
		return;
	}

	if (p < buf + len) {
		do_oack = true;
		parse_options(p, len - (p - buf), &blksize, &tsize, &wsize,
				&timeoutms, &rsize, &seek);
	}

	printf("[TQFTP] RRQ: %s (mode=%s rsize=%ld seek=%ld)\n", filename, mode, rsize, seek);

	sock = qrtr_open(0);
	if (sock < 0) {
		/* XXX: error */
		printf("[TQFTP] unable to create new qrtr socket, reject\n");
		return;
	}

	ret = connect(sock, (struct sockaddr *)sq, sizeof(*sq));
	if (ret < 0) {
		/* XXX: error */
		printf("[TQFTP] unable to connect new qrtr socket to remote\n");
		return;
	}

	fd = translate_open(filename, O_RDONLY);
	if (fd < 0) {
		printf("[TQFTP] unable to open %s (%d), reject\n", filename, errno);
		tftp_send_error(sock, 1, "file not found");
		return;
	}

	if (tsize != -1) {
		fstat(fd, &sb);
		tsize = sb.st_size;
	}

	client = calloc(1, sizeof(*client));
	client->sq = *sq;
	client->sock = sock;
	client->fd = fd;
	client->blksize = blksize;
	client->rsize = rsize;
	client->wsize = wsize;
	client->timeoutms = timeoutms;
	client->seek = seek;

	// printf("[TQFTP] new reader added\n");

	list_add(&readers, &client->node);

	if (do_oack) {
		tftp_send_oack(client->sock, &blksize,
			       tsize ? (size_t*)&tsize : NULL,
			       wsize ? &wsize : NULL,
			       &client->timeoutms,
			       rsize ? &rsize : NULL,
			       seek ? &seek : NULL);
	} else {
		tftp_send_data(client, 1, 0, 0);
	}
}

static void handle_wrq(const char *buf, size_t len, struct sockaddr_qrtr *sq)
{
	struct tftp_client *client;
	const char *filename;
	const char *mode;
	const char *p;
	ssize_t tsize = -1;
	size_t blksize = 512;
	unsigned int timeoutms = 1000;
	size_t rsize = 0;
	size_t wsize = 0;
	off_t seek = 0;
	bool do_oack = false;
	int sock;
	int ret;
	int fd;

	filename = buf + 2;
	mode = buf + 2 + strlen(filename) + 1;
	p = mode + strlen(mode) + 1;

	if (strcasecmp(mode, "octet")) {
		/* XXX: error */
		printf("[TQFTP] not octet, reject\n");
		return;
	}

	printf("[TQFTP] WRQ: %s (%s)\n", filename, mode);

	if (p < buf + len) {
		do_oack = true;
		parse_options(p, len - (p - buf), &blksize, &tsize, &wsize,
				&timeoutms, &rsize, &seek);
	}

	fd = translate_open(filename, O_WRONLY | O_CREAT);
	if (fd < 0) {
		/* XXX: error */
		printf("[TQFTP] unable to open %s (%d), reject\n", filename, errno);
		return;
	}

	sock = qrtr_open(0);
	if (sock < 0) {
		/* XXX: error */
		printf("[TQFTP] unable to create new qrtr socket, reject\n");
		return;
	}

	ret = connect(sock, (struct sockaddr *)sq, sizeof(*sq));
	if (ret < 0) {
		/* XXX: error */
		printf("[TQFTP] unable to connect new qrtr socket to remote\n");
		return;
	}

	client = calloc(1, sizeof(*client));
	client->sq = *sq;
	client->sock = sock;
	client->fd = fd;
	client->blksize = blksize;
	client->rsize = rsize;
	client->wsize = wsize;
	client->timeoutms = timeoutms;
	client->seek = seek;

	// printf("[TQFTP] new writer added\n");

	list_add(&writers, &client->node);

	if (do_oack) {
		tftp_send_oack(client->sock, &blksize,
			       tsize ? (size_t*)&tsize : NULL,
			       wsize ? &wsize : NULL,
			       &client->timeoutms,
			       rsize ? &rsize : NULL,
			       seek ? &seek : NULL);
	} else {
		tftp_send_data(client, 1, 0, 0);
	}
}

static int handle_reader(struct tftp_client *client)
{
	struct sockaddr_qrtr sq;
	uint16_t block;
	uint16_t last;
	char buf[128];
	socklen_t sl;
	ssize_t len;
	ssize_t n = 0;
	int opcode;
	int ret;

	sl = sizeof(sq);
	len = recvfrom(client->sock, buf, sizeof(buf), 0, (void *)&sq, &sl);
	if (len < 0) {
		ret = -errno;
		if (ret != -ENETRESET)
			fprintf(stderr, "[TQFTP] recvfrom failed: %d\n", ret);
		return -1;
	}

	/* Drop unsolicited messages */
	if (sq.sq_node != client->sq.sq_node ||
	    sq.sq_port != client->sq.sq_port) {
		printf("[TQFTP] Discarding spoofed message\n");
		return -1;
	}

	opcode = buf[0] << 8 | buf[1];
	if (opcode == OP_ERROR) {
		buf[len] = '\0';
		int err = buf[2] << 8 | buf[3];
		/* "End of Transfer" is not an error, used with stat(2)-like calls */
		if (err == ERROR_END_OF_TRANSFER)
			printf("[TQFTP] Remote returned END OF TRANSFER: %d - %s\n", err, buf + 4);
		else
			printf("[TQFTP] Remote returned an error: %d - %s\n", err, buf + 4);
		return -1;
	} else if (opcode != OP_ACK) {
		printf("[TQFTP] Expected ACK, got %d\n", opcode);
		return -1;
	}

	last = buf[2] << 8 | buf[3];
	// printf("[TQFTP] Got ack for %d\n", last);

	/* We've sent enough data for rsize already */
	if (last * client->blksize > client->rsize)
		return 0;

	for (block = last; block < last + client->wsize; block++) {
		size_t offset = client->seek + block * client->blksize;
		size_t response_size = 0;
		/* Check if need to limit response size based for requested rsize */
		if ((block + 1) * client->blksize > client->rsize)
			response_size = client->rsize % client->blksize;

		n = tftp_send_data(client, block + 1,
				   offset, response_size);
		if (n < 0) {
			printf("[TQFTP] Sent block %d failed: %zd\n", block + 1, n);
			break;
		}
		// printf("[TQFTP] Sent block %d of %zd\n", block + 1, n);
		if (n == 0)
			break;
		/* We've sent enough data for rsize already */
		if ((block + 1) * client->blksize > client->rsize)
			break;
	}

	return 1;
}

static int handle_writer(struct tftp_client *client)
{
	struct sockaddr_qrtr sq;
	uint16_t block;
	size_t payload;
	char buf[516];
	socklen_t sl;
	ssize_t len;
	int opcode;
	int ret;

	sl = sizeof(sq);
	len = recvfrom(client->sock, buf, sizeof(buf), 0, (void *)&sq, &sl);
	if (len < 0) {
		ret = -errno;
		if (ret != -ENETRESET)
			fprintf(stderr, "[TQFTP] recvfrom failed: %d\n", ret);
		return -1;
	}

	/* Drop unsolicited messages */
	if (sq.sq_node != client->sq.sq_node ||
	    sq.sq_port != client->sq.sq_port)
		return -1;

	opcode = buf[0] << 8 | buf[1];
	block = buf[2] << 8 | buf[3];
	if (opcode != OP_DATA) {
		printf("[TQFTP] Expected DATA opcode, got %d\n", opcode);
		tftp_send_error(client->sock, 4, "Expected DATA opcode");
		return -1;
	}

	payload = len - 4;

	ret = write(client->fd, buf + 4, payload);
	if (ret < 0) {
		/* XXX: report error */
		printf("[TQFTP] failed to write data\n");
		return -1;
	}

	tftp_send_ack(client->sock, block);

	return payload == 512 ? 1 : 0;
}

static void client_close_and_free(struct tftp_client *client)
{
	list_del(&client->node);
	close(client->sock);
	close(client->fd);
	free(client);
}

int main(int argc, char **argv)
{
	struct tftp_client *client;
	struct tftp_client *next;
	struct sockaddr_qrtr sq;
	struct qrtr_packet pkt;
	socklen_t sl;
	ssize_t len;
	char buf[4096];
	fd_set rfds;
	int nfds;
	int opcode;
	int ret;
	int fd;

	fd = qrtr_open(0);
	if (fd < 0) {
		fprintf(stderr, "failed to open qrtr socket\n");
		exit(1);
	}

	ret = qrtr_publish(fd, 4096, 1, 0);
	if (ret < 0) {
		fprintf(stderr, "failed to publish service registry service\n");
		exit(1);
	}

	zstd_init();

	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		nfds = fd;

		list_for_each_entry(client, &writers, node) {
			FD_SET(client->sock, &rfds);
			nfds = MAX(nfds, client->sock);
		}

		list_for_each_entry(client, &readers, node) {
			FD_SET(client->sock, &rfds);
			nfds = MAX(nfds, client->sock);
		}

		ret = select(nfds + 1, &rfds, NULL, NULL, NULL);
		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			} else {
				fprintf(stderr, "select failed\n");
				break;
			}
		}

		list_for_each_entry_safe(client, next, &writers, node) {
			if (FD_ISSET(client->sock, &rfds)) {
				ret = handle_writer(client);
				if (ret <= 0)
					client_close_and_free(client);
			}
		}

		list_for_each_entry_safe(client, next, &readers, node) {
			if (FD_ISSET(client->sock, &rfds)) {
				ret = handle_reader(client);
				if (ret <= 0)
					client_close_and_free(client);
			}
		}

		if (FD_ISSET(fd, &rfds)) {
			sl = sizeof(sq);
			len = recvfrom(fd, buf, sizeof(buf), 0, (void *)&sq, &sl);
			if (len < 0) {
				ret = -errno;
				if (ret != -ENETRESET)
					fprintf(stderr, "[TQFTP] recvfrom failed: %d\n", ret);
				return ret;
			}

			/* Ignore control messages */
			if (sq.sq_port == QRTR_PORT_CTRL) {
				ret = qrtr_decode(&pkt, buf, len, &sq);
				if (ret < 0) {
					fprintf(stderr, "[TQFTP] unable to decode qrtr packet\n");
					return ret;
				}

				switch (pkt.type) {
				case QRTR_TYPE_BYE:
					// fprintf(stderr, "[TQFTP] got bye\n");
					list_for_each_entry_safe(client, next, &writers, node) {
						if (client->sq.sq_node == sq.sq_node)
							client_close_and_free(client);
					}
					break;
				case QRTR_TYPE_DEL_CLIENT:
					// fprintf(stderr, "[TQFTP] got del_client\n");
					list_for_each_entry_safe(client, next, &writers, node) {
						if (!memcmp(&client->sq, &sq, sizeof(sq)))
							client_close_and_free(client);
					}
					break;
				}
			} else {
				if (len < 2)
					continue;

				opcode = buf[0] << 8 | buf[1];
				switch (opcode) {
				case OP_RRQ:
					handle_rrq(buf, len, &sq);
					break;
				case OP_WRQ:
					// printf("[TQFTP] write\n");
					handle_wrq(buf, len, &sq);
					break;
				case OP_ERROR:
					buf[len] = '\0';
					printf("[TQFTP] received error: %d - %s\n", buf[2] << 8 | buf[3], buf + 4);
					break;
				default:
					printf("[TQFTP] unhandled op %d\n", opcode);
					break;
				}
			}
		}
	}

	close(fd);
	zstd_free();

	return 0;
}
