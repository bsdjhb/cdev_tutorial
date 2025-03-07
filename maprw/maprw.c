#include <sys/param.h>
#include <sys/mman.h>
#include <err.h>
#include <fcntl.h>
#include <libutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr, "Usage: maprw <read|write> <file> <len> [offset]\n");
	exit(1);
}

int
main(int argc, char **argv)
{
	enum { READ, WRITE } mode;
	uint64_t val;
	off_t offset;
	size_t len, map_len, page_off, page_size;
	ssize_t done;
	void *p;
	int fd;

	if (argc < 4 || argc > 5)
		usage();

	if (strcmp(argv[1], "read") == 0) {
		mode = READ;
	} else if (strcmp(argv[1], "write") == 0) {
		mode = WRITE;
	} else
		usage();

	fd = open(argv[2], mode == READ ? O_RDONLY : O_WRONLY);
	if (fd == -1)
		err(1, "open(%s)", argv[2]);

	if (expand_number(argv[3], &val) == -1)
		err(1, "Failed to parse length");
	len = val;

	if (argc == 5) {
		if (expand_number(argv[4], &val) == -1)
			err(1, "Failed to parse offset");
		offset = val;
	} else
		offset = 0;

	/* Roundup (offset,length) to page alignment. */
	page_size = sysconf(_SC_PAGESIZE);
	page_off = offset % page_size;
	offset -= page_off;
	map_len = roundup2(page_off + len, page_size);

	p = mmap(NULL, map_len, mode == READ ? PROT_READ : PROT_WRITE,
	    MAP_SHARED, fd, offset);
	if (p == MAP_FAILED)
		err(1, "mmap");

	/*
	 * Note that the kernel will fault in the pages in the system
	 * call.  Also, a READ operation reads data from the mapping
	 * and writes to stdout while a WRITE operation reads data
	 * from stdin and stores it in the mapping.
	 */
	if (mode == READ) {
		done = write(STDOUT_FILENO, (const char *)p + page_off, len);
		if (done == -1)
			err(1, "write");
		if (done != len)
			warnx("short write: %zd", done);
	} else {
		done = read(STDIN_FILENO, (char *)p + page_off, len);
		if (done == -1)
			err(1, "read");
		if (done == 0)
			warnx("empty read");
		else if (done != len)
			warnx("short read: %zd", done);
	}

	return (0);
}
