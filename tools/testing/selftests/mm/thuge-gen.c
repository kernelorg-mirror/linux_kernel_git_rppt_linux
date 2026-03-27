// SPDX-License-Identifier: GPL-2.0
/* Test selecting other page sizes for mmap/shmget.

   Before running this huge pages for each huge page size must have been
   reserved.
   For large pages beyond MAX_PAGE_ORDER (like 1GB on x86) boot options must
   be used. 1GB wouldn't be tested if it isn't available.
   Also shmmax must be increased.
   And you need to run as root to work around some weird permissions in shm.
   And nothing using huge pages should run in parallel.
   When the program aborts you may need to clean up the shm segments with
   ipcrm -m by hand, like this
   sudo ipcs | awk '$1 == "0x00000000" {print $2}' | xargs -n1 sudo ipcrm -m
   (warning this will remove all if someone else uses them) */

#define _GNU_SOURCE
#include <sys/mman.h>
#include <linux/mman.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <glob.h>
#include <assert.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include "vm_util.h"
#include "kselftest.h"
#include "hugepage_settings.h"

#if !defined(MAP_HUGETLB)
#define MAP_HUGETLB	0x40000
#endif

#define SHM_HUGETLB     04000   /* segment will use huge TLB pages */
#ifndef SHM_HUGE_SHIFT
#define SHM_HUGE_SHIFT  26
#endif

#define NUM_PAGESIZES   5
#define NUM_PAGES 4

unsigned long page_sizes[NUM_PAGESIZES];
int num_page_sizes;

int ilog2(unsigned long v)
{
	int l = 0;
	while ((1UL << l) < v)
		l++;
	return l;
}

void show(unsigned long ps)
{
	if (ps == getpagesize())
		return;

	ksft_print_msg("%luMB: %ld\n", ps >> 20, hugetlb_free_pages(ps));
}

void test_mmap(unsigned long size, unsigned flags)
{
	char *map;
	unsigned long before, after;

	before = hugetlb_free_pages(size);
	map = mmap(NULL, size*NUM_PAGES, PROT_READ|PROT_WRITE,
			MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB|flags, -1, 0);
	if (map == MAP_FAILED)
		ksft_exit_fail_perror("mmap");

	memset(map, 0xff, size*NUM_PAGES);
	after = hugetlb_free_pages(size);

	show(size);
	ksft_test_result(size == getpagesize() || (before - after) == NUM_PAGES,
			 "%s mmap %lu %x\n", __func__, size, flags);

	if (munmap(map, size * NUM_PAGES))
		ksft_exit_fail_perror("unmap");
}

void test_shmget(unsigned long size, unsigned flags)
{
	int id;
	unsigned long before, after;
	struct shm_info i;
	char *map;

	before = hugetlb_free_pages(size);
	id = shmget(IPC_PRIVATE, size * NUM_PAGES, IPC_CREAT|0600|flags);
	if (id < 0) {
		if (errno == EPERM) {
			ksft_test_result_skip("shmget requires root privileges: %s\n",
					      strerror(errno));
			return;
		}
		ksft_exit_fail_perror("shmget");
	}

	if (shmctl(id, SHM_INFO, (void *)&i) < 0)
		ksft_exit_fail_perror("shmctl");

	map = shmat(id, NULL, 0600);
	if (map == MAP_FAILED)
		ksft_exit_fail_perror("shmat");

	shmctl(id, IPC_RMID, NULL);

	memset(map, 0xff, size*NUM_PAGES);
	after = hugetlb_free_pages(size);

	show(size);
	ksft_test_result(size == getpagesize() || (before - after) == NUM_PAGES,
			 "%s: mmap %lu %x\n", __func__, size, flags);
	if (shmdt(map))
		ksft_exit_fail_perror("shmdt");
}

void find_pagesizes(void)
{
	unsigned long largest = getpagesize();
	unsigned long shmmax_val = 0;
	int i;
	glob_t g;

	glob("/sys/kernel/mm/hugepages/hugepages-*kB", 0, NULL, &g);
	assert(g.gl_pathc <= NUM_PAGESIZES);
	for (i = 0; (i < g.gl_pathc) && (num_page_sizes < NUM_PAGESIZES); i++) {
		sscanf(g.gl_pathv[i], "/sys/kernel/mm/hugepages/hugepages-%lukB",
				&page_sizes[num_page_sizes]);
		page_sizes[num_page_sizes] <<= 10;
		ksft_print_msg("Found %luMB\n", page_sizes[i] >> 20);

		if (page_sizes[num_page_sizes] > largest)
			largest = page_sizes[i];

		if (hugetlb_free_pages(page_sizes[num_page_sizes]) >= NUM_PAGES)
			num_page_sizes++;
		else
			ksft_print_msg("SKIP for size %lu MB as not enough huge pages, need %u\n",
				       page_sizes[num_page_sizes] >> 20, NUM_PAGES);
	}
	globfree(&g);

	read_sysfs("/proc/sys/kernel/shmmax", &shmmax_val);
	if (shmmax_val < NUM_PAGES * largest) {
		ksft_print_msg("WARNING: shmmax is too small to run this test.\n");
		ksft_print_msg("Please run the following command to increase shmmax:\n");
		ksft_print_msg("echo %lu > /proc/sys/kernel/shmmax\n", largest * NUM_PAGES);
		ksft_exit_skip("Test skipped due to insufficient shmmax value.\n");
	}

#if defined(__x86_64__)
	if (largest != 1U<<30) {
		ksft_exit_skip("No GB pages available on x86-64\n"
				   "Please boot with hugepagesz=1G hugepages=%d\n", NUM_PAGES);
	}
#endif
}

int main(void)
{
	unsigned default_hps = default_huge_page_size();
	int i;

	ksft_print_header();

	find_pagesizes();

	if (!num_page_sizes)
		ksft_finished();

	ksft_set_plan(2 * num_page_sizes + 3);

	for (i = 0; i < num_page_sizes; i++) {
		unsigned long ps = page_sizes[i];
		int arg = ilog2(ps) << MAP_HUGE_SHIFT;

		ksft_print_msg("Testing %luMB mmap with shift %x\n", ps >> 20, arg);
		test_mmap(ps, MAP_HUGETLB | arg);
	}

	ksft_print_msg("Testing default huge mmap\n");
	test_mmap(default_hps, MAP_HUGETLB);

	ksft_print_msg("Testing non-huge shmget\n");
	test_shmget(getpagesize(), 0);

	for (i = 0; i < num_page_sizes; i++) {
		unsigned long ps = page_sizes[i];
		int arg = ilog2(ps) << SHM_HUGE_SHIFT;
		ksft_print_msg("Testing %luMB shmget with shift %x\n", ps >> 20, arg);
		test_shmget(ps, SHM_HUGETLB | arg);
	}

	ksft_print_msg("default huge shmget\n");
	test_shmget(default_hps, SHM_HUGETLB);

	ksft_finished();
}
