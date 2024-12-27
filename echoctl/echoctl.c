/*-
 * Copyright (c) 2024 John Baldwin <jhb@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysdecode.h>
#include <unistd.h>

#include <echodev.h>

static void
usage(void)
{
	fprintf(stderr, "Usage: echoctl <command> ...\n"
	    "\n"
	    "Where command is one of:\n"
	    "\tclear\t\t- clear buffer contents\n"
	    "\tevents [-rwW]\t- display I/O status events\n"
	    "\tpoll [-rwW]\t- display I/O status\n"
	    "\tresize <size>\t- set buffer size\n"
	    "\tsize\t\t- display buffer size\n");
	exit(1);
}

static int
open_device(int flags)
{
	int fd;

	fd = open("/dev/echo", flags);
	if (fd == -1)
		err(1, "/dev/echo");
	return (fd);
}

static void
clear(int argc, char **argv)
{
	int fd;

	if (argc != 2)
		usage();

	fd = open_device(O_RDWR);
	if (ioctl(fd, ECHODEV_CLEAR) == -1)
		err(1, "ioctl(ECHODEV_CLEAR)");
	close(fd);
}

static void
status(int argc, char **argv)
{
	struct pollfd pfd;
	int ch, count, events, fd;
	bool wait;

	argc--;
	argv++;

	events = 0;
	wait = false;
	while ((ch = getopt(argc, argv, "rwW")) != -1) {
		switch (ch) {
		case 'r':
			events |= POLLIN;
			break;
		case 'w':
			events |= POLLOUT;
			break;
		case 'W':
			wait = true;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;
	if (argc != 0)
		usage();

	if (events == 0)
		events = POLLIN | POLLOUT;

	fd = open_device(O_RDONLY);
	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	if (poll(&pfd, 1, wait ? INFTIM : 0) == -1)
		err(1, "poll");

	printf("Returned events: ");
	if (!sysdecode_pollfd_events(stdout, pfd.revents, NULL))
		printf("<none>");
	printf("\n");
	if (pfd.revents & POLLIN) {
		if (ioctl(fd, FIONREAD, &count) == -1)
			err(1, "ioctl(FIONREAD)");
		printf("%d bytes available to read\n", count);
	}
	if (pfd.revents & POLLOUT) {
		if (ioctl(fd, FIONWRITE, &count) == -1)
			err(1, "ioctl(FIONWRITE)");
		printf("room to write %d bytes\n", count);
	}

	close(fd);
}

static void
display_event(struct kevent *kev)
{
	printf("%s: ", sysdecode_kevent_filter(kev->filter));
	if (sysdecode_kevent_flags(stdout, kev->flags & ~EV_CLEAR, NULL))
		printf(" ");
	if (kev->flags & EV_ERROR)
		printf(" error: %s", strerror(kev->data));
	else
		printf("%jd bytes\n", (intmax_t)kev->data);
}

static void
events(int argc, char **argv)
{
	static struct timespec ts0;
	struct kevent kev;
	struct timespec *ts;
	int ch, events, fd, kq;
	bool wait;

	argc--;
	argv++;

	events = 0;
	wait = false;
	while ((ch = getopt(argc, argv, "rwW")) != -1) {
		switch (ch) {
		case 'r':
			events |= POLLIN;
			break;
		case 'w':
			events |= POLLOUT;
			break;
		case 'W':
			wait = true;
			break;
		default:
			usage();
		}
	}

	argc -= optind;
	argv += optind;
	if (argc != 0)
		usage();

	if (events == 0)
		events = POLLIN | POLLOUT;

	kq = kqueue();
	if (kq == -1)
		err(1, "kqueue");
	fd = open_device(O_RDONLY);
	if ((events & POLLIN) != 0) {
		EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
		if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1)
			err(1, "kevent(EVFILT_READ, EV_ADD)");
	}
	if ((events & POLLOUT) != 0) {
		EV_SET(&kev, fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, NULL);
		if (kevent(kq, &kev, 1, NULL, 0, NULL) == -1)
			err(1, "kevent(EVFILT_WRITE, EV_ADD)");
	}

	if (wait)
		ts = NULL;
	else
		ts = &ts0;
	for (;;) {
		events = kevent(kq, NULL, 0, &kev, 1, ts);
		if (events == -1)
			err(1, "kevent");
		if (events == 0)
			break;
		else
			display_event(&kev);
	}

	close(fd);
	close(kq);
}

static void
resize(int argc, char **argv)
{
	const char *errstr;
	size_t len;
	int fd;

	if (argc != 3)
		usage();

	len = (size_t)strtonum(argv[2], 0, 1024, &errstr);
	if (errstr != NULL)
		err(1, "new size is %s", errstr);

	fd = open_device(O_RDWR);
	if (ioctl(fd, ECHODEV_SBUFSIZE, &len) == -1)
		err(1, "ioctl(ECHODEV_SBUFSIZE)");
	close(fd);
}

static void
size(int argc, char **argv)
{
	size_t len;
	int fd;

	if (argc != 2)
		usage();

	fd = open_device(O_RDONLY);
	if (ioctl(fd, ECHODEV_GBUFSIZE, &len) == -1)
		err(1, "ioctl(ECHODEV_GBUFSIZE)");
	close(fd);

	printf("%zu\n", len);
}

int
main(int argc, char **argv)
{
	if (argc < 2)
		usage();

	if (strcmp(argv[1], "clear") == 0)
		clear(argc, argv);
	else if (strcmp(argv[1], "events") == 0)
		events(argc, argv);
	else if (strcmp(argv[1], "poll") == 0)
		status(argc, argv);
	else if (strcmp(argv[1], "resize") == 0)
		resize(argc, argv);
	else if (strcmp(argv[1], "size") == 0)
		size(argc, argv);
	else
		usage();

	return (0);
}
