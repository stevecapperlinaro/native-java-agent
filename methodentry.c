/*
 * native-java-agent
 *
 * Copyright (C) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Steve Capper <steve.capper@linaro.org>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "methodentry.h"

struct methodentry
{
	intptr_t start;
	uintptr_t size;
	struct methodentry *next;
	char name[];
};

static volatile struct methodentry *tail;
static volatile int methodcount;

/*
 * We may get multiple calls to me_addmethods from different threads.
 * So we atomicise access to the methodcount and tail global variables.
 *
 * All the rest of the me_ methods are called from a single thread.
 */
void me_addmethod(uintptr_t start, uintptr_t size, const char *format, ...)
{
	va_list ap;
	int numchars;
	struct methodentry *newentry, *expectedtail;

	va_start(ap, format);
	numchars = vsnprintf(NULL, 0, format, ap);
	va_end(ap);

	if (numchars <= 0) {
		fprintf(stderr, "Unexpected error in vsnprintf! ret = %d\n", numchars);
		return;
	}

	newentry = malloc(sizeof(struct methodentry) + numchars + 1);
	if (!newentry) {
		fprintf(stderr, "FATAL: Unable to allocate space for a methodentry!\n");
		return;
	}

	newentry->start = start;
	newentry->size = size;

	va_start(ap, format);
	numchars = vsnprintf(newentry->name, numchars + 1, format, ap);
	va_end(ap);

	if (numchars <= 0) {
		fprintf(stderr, "Unexpected error in vsnprintf output! ret = %d\n", numchars);
		free(newentry);
		return;
	}

	/*
	 * We only need __ATOMIC_RELAXED below as we are not protecting
	 * access to any resources (i.e. we are not locking), all we want
	 * is atomic update of two variables (tail pointer and count).
	 */
	while(!__atomic_compare_exchange_n(&tail, &expectedtail, newentry, 0,
			__ATOMIC_RELAXED, __ATOMIC_RELAXED));

	newentry->next = expectedtail;
	__atomic_add_fetch(&methodcount, 1, __ATOMIC_RELAXED);
}

static int comparemethodentries(const void *arg1, const void *arg2)
{
	int retval;
	struct methodentry *entry1 = *(struct methodentry **)arg1;
	struct methodentry *entry2 = *(struct methodentry **)arg2;

	retval = (int)(entry1->start - entry2->start);
	if (retval == 0)
		retval = strcmp(entry1->name, entry2->name);

	return retval;
}

static void checkforoverlap(struct methodentry **array)
{
	int loop;
	for (loop = 1; loop < methodcount; loop++) {
		if ((array[loop - 1]->start + array[loop - 1]->size)
			> array[loop]->start) {
			fprintf(stderr, "Overlapping found for %s and %s\n",
				array[loop - 1]->name, array[loop]->name);
		}
	}
}

static void writeoutputfile(struct methodentry **array)
{
	FILE *myfile;
	char *filename;
	pid_t mypid = getpid();
	int loop, ret;

	ret = asprintf(&filename, "/tmp/perf-%d.map", mypid);
	if (ret == -1) {
		fprintf(stderr, "Unable to allocate space for filename: %s\n", strerror(errno));
		return;
	}

	myfile = fopen(filename, "w");
	if (!myfile) {
		fprintf(stderr, "Unable to open %s for writing\n", filename);
		goto out;
	}

	for (loop = 0; loop < methodcount; loop++) {
		fprintf(myfile, "%lx %x %s\n", array[loop]->start,
			array[loop]->size, array[loop]->name);
	}

	fclose(myfile);
	fprintf(stderr, "Native codegen data written out to: %s\n", filename);
out:
	free(filename);
}

void me_outputperfmap(void)
{
	int loop;
	struct methodentry *current = (struct methodentry *) tail;
	struct methodentry **array;

	if (!methodcount) {
		fprintf(stderr, "No native code was generated, finishing...\n");
		return;
	}

	array = malloc(sizeof(struct methodentry *) * methodcount);
	if (!array) {
		fprintf(stderr, "Unable to allocate array!\n");
		return;
	}

	for (loop = 0; loop < methodcount; loop++) {
		array[loop] = current;
		current = current->next;
	}

	qsort(array, methodcount, sizeof(array[0]), comparemethodentries);

	checkforoverlap(array);

	writeoutputfile(array);

	free(array);
}

void me_freemethodentries(void)
{
	struct methodentry *next = (struct methodentry *) tail;
	tail = NULL;

	while(next) {
		struct methodentry *freeme = next;
		next = next->next;
		free(freeme);
		--methodcount;
	}

	if (methodcount) {
		fprintf(stderr, "Unexpected number of unfreed methodentries = %d\n", methodcount);
	}
}
