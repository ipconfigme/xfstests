/*
 * Copyright (C) 2011 Oracle.  All rights reserved.
 * Copyright (C) 2011 Red Hat.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _XOPEN_SOURCE 500
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

#ifndef SEEK_DATA
#define SEEK_DATA      3
#define SEEK_HOLE      4
#endif

static blksize_t alloc_size;
int default_behavior = 0;
char *base_file_path;

static void get_file_system(int fd)
{
	struct statfs buf;

	if (!fstatfs(fd, &buf))
		fprintf(stdout, "File system magic#: 0x%lx\n", buf.f_type);
}

static int get_io_sizes(int fd)
{
       struct stat buf;
       int ret;

       ret = fstat(fd, &buf);
       if (ret)
               fprintf(stderr, "  ERROR %d: Failed to find io blocksize\n",
                       errno);

       /* st_blksize is typically also the allocation size */
       alloc_size = buf.st_blksize;
       fprintf(stdout, "Allocation size: %ld\n", alloc_size);

       return ret;
}

#define do_free(x)     do { if(x) free(x); } while(0);

static void *do_malloc(size_t size)
{
       void *buf;

       buf = malloc(size);
       if (!buf)
               fprintf(stderr, "  ERROR: Unable to allocate %ld bytes\n",
                       (long)size);

       return buf;
}

static int do_truncate(int fd, off_t length)
{
       int ret;

       ret = ftruncate(fd, length);
       if (ret)
               fprintf(stderr, "  ERROR %d: Failed to extend file "
                       "to %ld bytes\n", errno, (long)length);
       return ret;
}

static int do_fallocate(int fd, off_t offset, off_t length, int mode)
{
	int ret;

	ret = fallocate(fd, mode, offset, length);
	if (ret)
		fprintf(stderr, "  ERROR %d: Failed to preallocate "
			"space to %ld bytes\n", errno, (long) length);

	return ret;
}

/*
 * Synchnorize all dirty pages in the file range starting from
 * offset to nbytes length.
 */
static int do_sync_dirty_pages(int fd, off64_t offset, off64_t nbytes)
{
	int ret;

	ret = sync_file_range(fd, offset, nbytes, SYNC_FILE_RANGE_WRITE);
	if (ret)
		fprintf(stderr, "  ERROR %d: Failed to sync out dirty "
			"pages\n", errno);

	return ret;
}

static ssize_t do_pwrite(int fd, const void *buf, size_t count, off_t offset)
{
       ssize_t ret, written = 0;

       while (count > written) {
               ret = pwrite(fd, buf + written, count - written, offset + written);
               if (ret < 0) {
                       fprintf(stderr, "  ERROR %d: Failed to write %ld "
                               "bytes\n", errno, (long)count);
                       return ret;
               }
               written += ret;
       }

       return 0;
}

#define do_close(x)	do { if ((x) > -1) close(x); } while(0);

static int do_create(const char *filename)
{
	int fd;

	fd = open(filename, O_RDWR|O_CREAT|O_TRUNC, 0644);
	if (fd < 0)
		fprintf(stderr, " ERROR %d: Failed to create file '%s'\n",
			errno, filename);

	return fd;
}

static int do_lseek(int testnum, int subtest, int fd, int filsz, int origin,
		    off_t set, off_t exp)
{
	off_t pos, exp2;
	int x, ret;

	assert(!(origin != SEEK_HOLE && origin != SEEK_DATA));

	/*
	 * The file pointer can be set to different values depending
	 * on the implementation. For SEEK_HOLE, EOF could be a valid
	 * value. For SEEK_DATA, supplied offset could be the valid
	 * value.
	 */
	exp2 = exp;
	if (origin == SEEK_HOLE && exp2 != -1)
		exp2 = filsz;
	if (origin == SEEK_DATA && default_behavior && set < filsz)
		exp2 = set;

	pos = lseek(fd, set, origin);

	if (pos == -1 && exp == -1) {
		x = fprintf(stdout, "%02d.%02d %s expected -1 with errno %d, got %d. ",
			    testnum, subtest,
			    (origin == SEEK_HOLE) ? "SEEK_HOLE" : "SEEK_DATA",
			    -ENXIO, -errno);
		ret = !(errno == ENXIO);
	} else {

		x = fprintf(stdout, "%02d.%02d %s expected %ld or %ld, got %ld. ",
			    testnum, subtest,
			    (origin == SEEK_HOLE) ? "SEEK_HOLE" : "SEEK_DATA",
			    (long)exp, (long)exp2, (long)pos);
		ret = !(pos == exp || pos == exp2);
	}

	fprintf(stdout, "%*s\n", (70 - x), ret ? "FAIL" : "succ");

	return ret;
}

/*
 * test file with unwritten extents, have both dirty and
 * writeback pages in page cache.
 */
static int test09(int fd, int testnum)
{
	int ret = 0;
	char *buf = NULL;
	int bufsz = alloc_size;
	int filsz = 8 << 20;

	/*
	 * HOLE - unwritten DATA in dirty page - HOLE -
	 * unwritten DATA in writeback page
	 */

	/* Each unit is bufsz */
	buf = do_malloc(bufsz);
	if (!buf)
		goto out;
	memset(buf, 'a', bufsz);

	/* preallocate 8M space to file */
	ret = do_fallocate(fd, 0, filsz, 0);
	if (ret < 0)
		goto out;

	ret = do_pwrite(fd, buf, bufsz, bufsz * 10);
	if (!ret) {
		ret = do_pwrite(fd, buf, bufsz, bufsz * 100);
		if (ret)
			goto out;
	}

	/*
	 * Sync out dirty pages from bufsz * 100, this will convert
	 * the dirty page to writeback.
	 */
	ret = do_sync_dirty_pages(fd, bufsz * 100, 0);
	if (ret)
		goto out;

	/* offset at the beginning */
	ret += do_lseek(testnum,  1, fd, filsz, SEEK_HOLE, 0, 0);
	ret += do_lseek(testnum,  2, fd, filsz, SEEK_HOLE, 1, 1);
	ret += do_lseek(testnum,  3, fd, filsz, SEEK_DATA, 0, bufsz * 10);
	ret += do_lseek(testnum,  4, fd, filsz, SEEK_DATA, 1, bufsz * 10);

out:
	do_free(buf);
	return ret;
}

/* test file with unwritten extent, only have writeback page */
static int test08(int fd, int testnum)
{
	int ret = 0;
	char *buf = NULL;
	int bufsz = alloc_size;
	int filsz = 4 << 20;

	/* HOLE - unwritten DATA in writeback page */
	/* Each unit is bufsz */
	buf = do_malloc(bufsz);
	if (!buf)
		goto out;
	memset(buf, 'a', bufsz);

	/* preallocate 4M space to file */
	ret = do_fallocate(fd, 0, filsz, 0);
	if (ret < 0)
		goto out;

	ret = do_pwrite(fd, buf, bufsz, bufsz * 10);
	if (ret)
		goto out;

	/* Sync out all file */
	ret = do_sync_dirty_pages(fd, 0, 0);
	if (ret)
		goto out;

	/* offset at the beginning */
	ret += do_lseek(testnum,  1, fd, filsz, SEEK_HOLE, 0, 0);
	ret += do_lseek(testnum,  2, fd, filsz, SEEK_HOLE, 1, 1);
	ret += do_lseek(testnum,  3, fd, filsz, SEEK_DATA, 0, bufsz * 10);
	ret += do_lseek(testnum,  4, fd, filsz, SEEK_DATA, 1, bufsz * 10);

out:
	do_free(buf);
	return ret;
}

/*
 * test file with unwritten extents, only have dirty pages
 * in page cache.
 */
static int test07(int fd, int testnum)
{
	int ret = 0;
	char *buf = NULL;
	int bufsz = alloc_size;
	int filsz = 4 << 20;

	/* HOLE - unwritten DATA in dirty page */
	/* Each unit is bufsz */
	buf = do_malloc(bufsz);
	if (!buf)
		goto out;
	memset(buf, 'a', bufsz);

	/* preallocate 4M space to file */
	ret = do_fallocate(fd, 0, filsz, 0);
	if (ret < 0)
		goto out;

	ret = do_pwrite(fd, buf, bufsz, bufsz * 10);
	if (ret)
		goto out;

	/* offset at the beginning */
	ret += do_lseek(testnum,  1, fd, filsz, SEEK_HOLE, 0, 0);
	ret += do_lseek(testnum,  2, fd, filsz, SEEK_HOLE, 1, 1);
	ret += do_lseek(testnum,  3, fd, filsz, SEEK_DATA, 0, bufsz * 10);
	ret += do_lseek(testnum,  4, fd, filsz, SEEK_DATA, 1, bufsz * 10);

out:
	do_free(buf);
	return ret;
}

/* test hole data hole data */
static int test06(int fd, int testnum)
{
	int ret = -1;
	char *buf = NULL;
	int bufsz = alloc_size;
	int filsz = bufsz * 4;
	int off;

	/* HOLE - DATA - HOLE - DATA */
	/* Each unit is bufsz */

	buf = do_malloc(bufsz);
	if (!buf)
		goto out;

	memset(buf, 'a', bufsz);

	ret = do_pwrite(fd, buf, bufsz, bufsz);
	if (!ret)
		do_pwrite(fd, buf, bufsz, bufsz * 3);
	if (ret)
		goto out;

	/* offset at the beginning */
	ret += do_lseek(testnum,  1, fd, filsz, SEEK_HOLE, 0, 0);
	ret += do_lseek(testnum,  2, fd, filsz, SEEK_HOLE, 1, 1);
	ret += do_lseek(testnum,  3, fd, filsz, SEEK_DATA, 0, bufsz);
	ret += do_lseek(testnum,  4, fd, filsz, SEEK_DATA, 1, bufsz);

	/* offset around first hole-data boundary */
	off = bufsz;
	ret += do_lseek(testnum,  5, fd, filsz, SEEK_HOLE, off - 1, off - 1);
	ret += do_lseek(testnum,  6, fd, filsz, SEEK_DATA, off - 1, off);
	ret += do_lseek(testnum,  7, fd, filsz, SEEK_HOLE, off,     bufsz * 2);
	ret += do_lseek(testnum,  8, fd, filsz, SEEK_DATA, off,     off);
	ret += do_lseek(testnum,  9, fd, filsz, SEEK_HOLE, off + 1, bufsz * 2);
	ret += do_lseek(testnum, 10, fd, filsz, SEEK_DATA, off + 1, off + 1);

	/* offset around data-hole boundary */
	off = bufsz * 2;
	ret += do_lseek(testnum, 11, fd, filsz, SEEK_HOLE, off - 1, off);
	ret += do_lseek(testnum, 12, fd, filsz, SEEK_DATA, off - 1, off - 1);
	ret += do_lseek(testnum, 13, fd, filsz, SEEK_HOLE, off,     off);
	ret += do_lseek(testnum, 14, fd, filsz, SEEK_DATA, off,     bufsz * 3);
	ret += do_lseek(testnum, 15, fd, filsz, SEEK_HOLE, off + 1, off + 1);
	ret += do_lseek(testnum, 16, fd, filsz, SEEK_DATA, off + 1, bufsz * 3);

	/* offset around second hole-data boundary */
	off = bufsz * 3;
	ret += do_lseek(testnum, 17, fd, filsz, SEEK_HOLE, off - 1, off - 1);
	ret += do_lseek(testnum, 18, fd, filsz, SEEK_DATA, off - 1, off);
	ret += do_lseek(testnum, 19, fd, filsz, SEEK_HOLE, off,     filsz);
	ret += do_lseek(testnum, 20, fd, filsz, SEEK_DATA, off,     off);
	ret += do_lseek(testnum, 21, fd, filsz, SEEK_HOLE, off + 1, filsz);
	ret += do_lseek(testnum, 22, fd, filsz, SEEK_DATA, off + 1, off + 1);

	/* offset around the end of file */
	off = filsz;
	ret += do_lseek(testnum, 23, fd, filsz, SEEK_HOLE, off - 1, filsz);
	ret += do_lseek(testnum, 24, fd, filsz, SEEK_DATA, off - 1, filsz - 1);
	ret += do_lseek(testnum, 25, fd, filsz, SEEK_HOLE, off, -1);
	ret += do_lseek(testnum, 26, fd, filsz, SEEK_DATA, off, -1);
	ret += do_lseek(testnum, 27, fd, filsz, SEEK_HOLE, off + 1, -1);
	ret += do_lseek(testnum, 28, fd, filsz, SEEK_DATA, off + 1, -1);

out:
	do_free(buf);
	return ret;
}

/* test file with data at the beginning and a hole at the end */
static int test05(int fd, int testnum)
{
	int ret = -1;
	char *buf = NULL;
	int bufsz = alloc_size;
	int filsz = bufsz * 4;

	/* |- DATA -|- HOLE -|- HOLE -|- HOLE -| */

	buf = do_malloc(bufsz);
	if (!buf)
		goto out;
	memset(buf, 'a', bufsz);

	ret = do_truncate(fd, filsz);
	if (!ret)
		ret = do_pwrite(fd, buf, bufsz, 0);
	if (ret)
		goto out;

	/* offset at the beginning */

	ret += do_lseek(testnum,  1, fd, filsz, SEEK_HOLE, 0, bufsz);
	ret += do_lseek(testnum,  2, fd, filsz, SEEK_HOLE, 1, bufsz);

	ret += do_lseek(testnum,  3, fd, filsz, SEEK_DATA, 0, 0);
	ret += do_lseek(testnum,  4, fd, filsz, SEEK_DATA, 1, 1);

	/* offset around data-hole boundary */
	ret += do_lseek(testnum,  5, fd, filsz, SEEK_HOLE, bufsz - 1, bufsz);
	ret += do_lseek(testnum,  6, fd, filsz, SEEK_DATA, bufsz - 1, bufsz - 1);

	ret += do_lseek(testnum,  7, fd, filsz, SEEK_HOLE, bufsz,     bufsz);
	ret += do_lseek(testnum,  8, fd, filsz, SEEK_DATA, bufsz,     -1);
	ret += do_lseek(testnum,  9, fd, filsz, SEEK_HOLE, bufsz + 1, bufsz + 1);
	ret += do_lseek(testnum, 10, fd, filsz, SEEK_DATA, bufsz + 1, -1);

	/* offset around eof */
	ret += do_lseek(testnum, 11, fd, filsz, SEEK_HOLE, filsz - 1, filsz - 1);
	ret += do_lseek(testnum, 12, fd, filsz, SEEK_DATA, filsz - 1, -1);
	ret += do_lseek(testnum, 13, fd, filsz, SEEK_HOLE, filsz,     -1);
	ret += do_lseek(testnum, 14, fd, filsz, SEEK_DATA, filsz,     -1);
	ret += do_lseek(testnum, 15, fd, filsz, SEEK_HOLE, filsz + 1, -1);
	ret += do_lseek(testnum, 16, fd, filsz, SEEK_DATA, filsz + 1, -1);
out:
	do_free(buf);
	return ret;
}
/* test hole begin and data end */
static int test04(int fd, int testnum)
{
	int ret;
	char *buf = "ABCDEFGH";
	int bufsz, holsz, filsz;

	bufsz = strlen(buf);
	holsz = alloc_size * 2;
	filsz = holsz + bufsz;

	/* |- HOLE -|- HOLE -|- DATA -| */

	ret = do_pwrite(fd, buf, bufsz, holsz);
	if (ret)
		goto out;

	/* offset at the beginning */
	ret += do_lseek(testnum,  1, fd, filsz, SEEK_HOLE, 0, 0);
	ret += do_lseek(testnum,  2, fd, filsz, SEEK_HOLE, 1, 1);
	ret += do_lseek(testnum,  3, fd, filsz, SEEK_DATA, 0, holsz);
	ret += do_lseek(testnum,  4, fd, filsz, SEEK_DATA, 1, holsz);
	/* offset around hole-data boundary */
	ret += do_lseek(testnum,  5, fd, filsz, SEEK_HOLE, holsz - 1, holsz - 1);
	ret += do_lseek(testnum,  6, fd, filsz, SEEK_DATA, holsz - 1, holsz);
	ret += do_lseek(testnum,  7, fd, filsz, SEEK_HOLE, holsz,     filsz);
	ret += do_lseek(testnum,  8, fd, filsz, SEEK_DATA, holsz,     holsz);
	ret += do_lseek(testnum,  9, fd, filsz, SEEK_HOLE, holsz + 1, filsz);
	ret += do_lseek(testnum, 10, fd, filsz, SEEK_DATA, holsz + 1, holsz + 1);

	/* offset around eof */
	ret += do_lseek(testnum, 11, fd, filsz, SEEK_HOLE, filsz - 1, filsz);
	ret += do_lseek(testnum, 12, fd, filsz, SEEK_DATA, filsz - 1, filsz - 1);
	ret += do_lseek(testnum, 13, fd, filsz, SEEK_HOLE, filsz,     -1);
	ret += do_lseek(testnum, 14, fd, filsz, SEEK_DATA, filsz,     -1);
	ret += do_lseek(testnum, 15, fd, filsz, SEEK_HOLE, filsz + 1, -1);
	ret += do_lseek(testnum, 16, fd, filsz, SEEK_DATA, filsz + 1, -1);
out:
	return ret;
}

/* test a larger full file */
static int test03(int fd, int testnum)
{
	char *buf = NULL;
	int bufsz = alloc_size * 2 + 100;
	int filsz = bufsz;
	int ret = -1;

	buf = do_malloc(bufsz);
	if (!buf)
		goto out;
	memset(buf, 'a', bufsz);

	ret = do_pwrite(fd, buf, bufsz, 0);
	if (ret)
		goto out;

	/* offset at the beginning */
	ret += do_lseek(testnum,  1, fd, filsz, SEEK_HOLE, 0, bufsz);
	ret += do_lseek(testnum,  2, fd, filsz, SEEK_HOLE, 1, bufsz);
	ret += do_lseek(testnum,  3, fd, filsz, SEEK_DATA, 0, 0);
	ret += do_lseek(testnum,  4, fd, filsz, SEEK_DATA, 1, 1);

	/* offset around eof */
	ret += do_lseek(testnum,  5, fd, filsz, SEEK_HOLE, bufsz - 1, bufsz);
	ret += do_lseek(testnum,  6, fd, filsz, SEEK_DATA, bufsz - 1, bufsz - 1);
	ret += do_lseek(testnum,  7, fd, filsz, SEEK_HOLE, bufsz,     -1);
	ret += do_lseek(testnum,  8, fd, filsz, SEEK_DATA, bufsz,     -1);
	ret += do_lseek(testnum,  9, fd, filsz, SEEK_HOLE, bufsz + 1, -1);
	ret += do_lseek(testnum, 10, fd, filsz, SEEK_DATA, bufsz + 1, -1);

out:
	do_free(buf);
	return ret;
}

/* test tiny full file */
static int test02(int fd, int testnum)
{
	int ret;
	char buf[] = "ABCDEFGH";
	int bufsz, filsz;

	bufsz = strlen(buf);
	filsz = bufsz;

	/* |- DATA -| */

	ret = do_pwrite(fd, buf, bufsz, 0);
	if (ret)
		goto out;

	ret += do_lseek(testnum, 1, fd, filsz, SEEK_HOLE, 0, filsz);
	ret += do_lseek(testnum, 2, fd, filsz, SEEK_DATA, 0, 0);
	ret += do_lseek(testnum, 3, fd, filsz, SEEK_DATA, 1, 1);
	ret += do_lseek(testnum, 4, fd, filsz, SEEK_HOLE, bufsz - 1, filsz);
	ret += do_lseek(testnum, 5, fd, filsz, SEEK_DATA, bufsz - 1, bufsz - 1);
	ret += do_lseek(testnum, 6, fd, filsz, SEEK_HOLE, bufsz,     -1);
	ret += do_lseek(testnum, 7, fd, filsz, SEEK_DATA, bufsz,     -1);
	ret += do_lseek(testnum, 8, fd, filsz, SEEK_HOLE, bufsz + 1, -1);
	ret += do_lseek(testnum, 9, fd, filsz, SEEK_DATA, bufsz + 1, -1);

out:
	return ret;
}

/* test empty file */
static int test01(int fd, int testnum)
{
	int ret = 0;

	ret += do_lseek(testnum, 1, fd, 0, SEEK_DATA, 0, -1);
	ret += do_lseek(testnum, 2, fd, 0, SEEK_HOLE, 0, -1);
	ret += do_lseek(testnum, 3, fd, 0, SEEK_HOLE, 1, -1);

	return ret;
}

struct testrec {
       int     test_num;
       int     (*test_func)(int fd, int testnum);
       char    *test_desc;
};

struct testrec seek_tests[] = {
       {  1, test01, "Test empty file" },
       {  2, test02, "Test a tiny full file" },
       {  3, test03, "Test a larger full file" },
       {  4, test04, "Test file hole at beg, data at end" },
       {  5, test05, "Test file data at beg, hole at end" },
       {  6, test06, "Test file hole data hole data" },
       {  7, test07, "Test file with unwritten extents, only have dirty pages" },
       {  8, test08, "Test file with unwritten extents, only have unwritten pages" },
       {  9, test09, "Test file with unwritten extents, have both dirty && unwritten pages" },
};

static int run_test(struct testrec *tr)
{
	int ret = 0, fd = -1;
	char filename[255];

	snprintf(filename, sizeof(filename), "%s%02d", base_file_path, tr->test_num);

	fd = do_create(filename);
	if (fd > -1) {
		printf("%02d. %-50s\n", tr->test_num, tr->test_desc);
		ret = tr->test_func(fd, tr->test_num);
		printf("\n");
	}

	do_close(fd);
	return ret;
}

static int test_basic_support(void)
{
	int ret = -1, fd;
	off_t pos;
	char *buf = NULL;
	int bufsz, filsz;

	fd = do_create(base_file_path);
	if (fd == -1)
		goto out;

	get_file_system(fd);

	ret = get_io_sizes(fd);
	if (ret)
		goto out;

	bufsz = alloc_size * 2;
	filsz = bufsz * 2;

	buf = do_malloc(bufsz);
	if (!buf)
		goto out;
	memset(buf, 'a', bufsz);

	/* File with 2 allocated blocks.... */
	ret = do_pwrite(fd, buf, bufsz, 0);
	if (ret)
		goto out;

	/* followed by a hole... */
	ret = do_truncate(fd, filsz);
	if (ret)
		goto out;

	/* Is SEEK_DATA and SEEK_HOLE supported in the kernel? */
	pos = lseek(fd, 0, SEEK_DATA);
	if (pos != -1)
		pos = lseek(fd, 0, SEEK_HOLE);
	if (pos == -1) {
		fprintf(stderr, "Kernel does not support llseek(2) extensions "
			"SEEK_HOLE and/or SEEK_DATA. Aborting.\n");
		ret = -1;
		goto out;
	}

	if (pos == filsz) {
		default_behavior = 1;
		fprintf(stderr, "File system supports the default behavior.\n");
	}

	printf("\n");

out:
	do_free(buf);
	do_close(fd);
	return ret;
}

int main(int argc, char **argv)
{
	int ret = -1;
	int i = 0;
	int numtests = sizeof(seek_tests) / sizeof(struct testrec);

	if (argc != 2) {
		fprintf(stdout, "Usage: %s base_file_path\n", argv[0]);
		return ret;
	}

	base_file_path = (char *)strdup(argv[1]);

	ret = test_basic_support();
	if (ret)
		goto out;

	for (i = 0; i < numtests; ++i) {
		if (ret)
			goto out;
		run_test(&seek_tests[i]);
	}

out:
	free(base_file_path);
	return ret;
}
