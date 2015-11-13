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
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <jni.h>
#include <jvmti.h>
#include <jvmticmlr.h>

#include "methodentry.h"

#define CHECKBADRET(x, y) tires = x; if(tires) \
	{ fprintf(stderr, "Unexpected return code from " #x ", on line %d got %d back\n", __LINE__, tires); \
		return y; }

static void getclassname(jvmtiEnv *jvmti, jclass klass, char **name)
{
	char *classname;
	int ret, loop;
	jvmtiError tires;
	*name = NULL;

	CHECKBADRET((*jvmti)->GetClassSignature(jvmti, klass, &classname, NULL),);

	for (loop = 1; loop < strlen(classname); loop++) {
		if (classname[loop] == '/')
			classname[loop] = '.';
		else if (classname[loop] == ';')
			classname[loop] = '\0';
	}

	ret = asprintf(name, "%s", classname + 1);
	if (ret == -1) {
		fprintf(stderr, "Unable to allocate space for classname: %s\n",
			strerror(errno));
		*name = NULL;
	}

	CHECKBADRET((*jvmti)->Deallocate(jvmti, classname),);
}

void decodelinenumber(jvmtiEnv *jvmti, FILE *output, jmethodID method, jint bci,
	const void *pc)
{
	jclass klass;
	jvmtiError tires;
	jint table_entry_count;
	jvmtiLineNumberEntry *linetable;
	char *methodname, *source_file_name, *classname;
	int tloop;

	CHECKBADRET((*jvmti)->GetMethodName(jvmti, method, &methodname, NULL, NULL),);
	CHECKBADRET((*jvmti)->GetMethodDeclaringClass(jvmti, method, &klass),);
	CHECKBADRET((*jvmti)->GetSourceFileName(jvmti, klass, &source_file_name),);

	CHECKBADRET((*jvmti)->GetLineNumberTable(jvmti, method, &table_entry_count,
			&linetable),);

	for(tloop = 1; tloop < table_entry_count; tloop++) {
		if (linetable[tloop].start_location > bci)
			break;
	}

	getclassname(jvmti, klass, &classname);

	fprintf(output, "0x%llx:%s:%s:%d:%s:%d\n", pc, classname, source_file_name,
		(bci == -1) ? 0 : linetable[tloop - 1].line_number,
		methodname, bci);
	free(classname);

	CHECKBADRET((*jvmti)->Deallocate(jvmti, source_file_name),);
	CHECKBADRET((*jvmti)->Deallocate(jvmti, (unsigned char*)linetable),);
	CHECKBADRET((*jvmti)->Deallocate(jvmti, methodname),);
}

void analyseinline(jvmtiEnv *jvmti, FILE *output, PCStackInfo *stack)
{
	int loop;
	for (loop = 0; loop < stack->numstackframes; loop++) {
		decodelinenumber(jvmti, output, stack->methods[loop],
			stack->bcis[loop], stack->pc);
	}
}

void analysecompileinfo(jvmtiEnv *jvmti, const void *code_addr,
		const char *name, const void *compile_info)
{
	int current = 0;
	char *filename;
	FILE *output;
	int res = asprintf(&filename, "/tmp/perf-%d.map.d/%lx.methodinfo",
			getpid(), (uintptr_t)code_addr);
	if (res == -1) {
		fprintf(stderr, "Couldn't allocate string %s\n",
			strerror(errno));
		return;
	}

	output = fopen(filename, "w");
	if (!output) {
		fprintf(stderr, "Unable to create %s\n", filename);
		goto out;
	}

	jvmtiCompiledMethodLoadRecordHeader *head =
		(jvmtiCompiledMethodLoadRecordHeader *) compile_info;

	while(head) {
		switch(head->kind) {
		case JVMTI_CMLR_DUMMY:
			{
			jvmtiCompiledMethodLoadDummyRecord *dummy =
				(jvmtiCompiledMethodLoadDummyRecord *)head;
			fprintf(output, "Type dummy: %s\n", dummy->message);
			break;
			}

		case JVMTI_CMLR_INLINE_INFO:
			{
			int loop;
			jvmtiCompiledMethodLoadInlineRecord *in =
				(jvmtiCompiledMethodLoadInlineRecord *)head;
			for (loop = 0; loop < in->numpcs; loop++) {
				analyseinline(jvmti, output, &in->pcinfo[loop]);
			}
			break;
			}
		default:
			fprintf(output, "Unknown compile_info type: %d\n", head->kind);
		}

		head = head->next;
		++current;
	}

	fclose(output);
out:
	free(filename);
}

static void dumpdata(const void *code_addr, jint code_size)
{
	char *filename;
	FILE *output;
	size_t written;
	int res = asprintf(&filename, "/tmp/perf-%d.map.d/%lx.dump", getpid(),
		(uintptr_t)code_addr);
	if (res == -1) {
		fprintf(stderr, "Couldn't allocate string %s\n", strerror(errno));
		return;
	}

	output = fopen(filename, "wb");
	if (!output) {
		fprintf(stderr, "Unable to create %s\n", filename);
		goto out;
	}

	written = fwrite(code_addr, code_size, 1, output);
	if (written != 1)
		fprintf(stderr, "Unable to write %d bytes to %s\n",
			code_size, filename);
	fclose(output);

out:
	free(filename);
}

static void _CompiledMethodLoad(jvmtiEnv *jvmti,
            jmethodID method,
            jint code_size,
            const void* code_addr,
            jint map_length,
            const jvmtiAddrLocationMap* map,
            const void* compile_info)
{
	char *name, *classname;
	jclass klass;

	jvmtiError tires;

	dumpdata(code_addr, code_size);

	CHECKBADRET((*jvmti)->GetMethodName(jvmti, method, &name, NULL, NULL),);

	analysecompileinfo(jvmti, code_addr, name, compile_info);

	CHECKBADRET((*jvmti)->GetMethodDeclaringClass(jvmti, method, &klass),);
	getclassname(jvmti, klass, &classname);

	me_addmethod((int64_t)code_addr, code_size, "%s:%s", classname, name);

	CHECKBADRET((*jvmti)->Deallocate(jvmti, name),);
}

static void _DynamicCodeGenerated(jvmtiEnv *jvmti_env,
            const char* name,
            const void* address,
            jint length)
{
	dumpdata(address, length);
	me_addmethod((int64_t) address, length, "%s DYNAMIC", name);
}


/*
 * The JVM will fire DynamicCodeGenerated and CompileMethodInfo events from
 * multiple threads. It will then fire a VMDeath event just before shutdown,
 * and we need to block in VMDeath until all our previous events have completed.
 * Also, we want to check for events fired after we have finished with VMDeath
 * as this means we've forgotten to call SetEventNotificationMode and
 * deactivate the events.
 *
 * This logic is achieved with a basic integer count of active threads.
 * If this count is >0, that is the number of events being actively processed.
 * If the count is 0 then we can proceed through VMDeath.
 * After VMDeath, count is INT_MIN meaning that any events that fire detect
 * a negative count and show an error.
 */

static volatile int _agentsactive;

static inline int incrementagents(void)
{
	return __atomic_add_fetch(&_agentsactive, 1, __ATOMIC_ACQUIRE);
}

static inline void decrementagents(void)
{
	__atomic_sub_fetch(&_agentsactive, 1, __ATOMIC_RELEASE);
}

void JNICALL
DynamicCodeGenerated(jvmtiEnv *jvmti_env,
            const char* name,
            const void* address,
            jint length)
{
	if (incrementagents() < 0) {
		fprintf(stderr, "Unexpected shutdown received before dynamic code could start\n");
		return;
	}
	_DynamicCodeGenerated(jvmti_env, name, address, length);
	decrementagents();
}

void JNICALL
CompiledMethodLoad(jvmtiEnv *jvmti,
            jmethodID method,
            jint code_size,
            const void* code_addr,
            jint map_length,
            const jvmtiAddrLocationMap* map,
            const void* compile_info)
{
	if (incrementagents() < 0) {
		fprintf(stderr, "Unexpected shutdown received before methodload could start\n");
		return;
	}

	_CompiledMethodLoad(jvmti, method, code_size, code_addr,
			map_length, map, compile_info);
	decrementagents();
}

void JNICALL
VMDeath(jvmtiEnv *jvmti,
            JNIEnv* jni_env)
{
	int expected = 0;
	jvmtiError tires;

	CHECKBADRET((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL),);
	CHECKBADRET((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL),);

	/* spin until we've finished decoding everything */
	while(!__atomic_compare_exchange_n(&_agentsactive, &expected, INT_MIN,
		0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		expected = 0;
	}
}


/*
 * Entry point, get everything ready for our tracing.
 */
JNIEXPORT jint JNICALL
Agent_OnLoad(JavaVM* vm, char *options, void *reserved)

{
	jvmtiEnv *jvmti;
	jvmtiError tires;
	pid_t pid;
	char *dirname;
	int md, ret;

	jvmtiEventCallbacks mycallbacks = {
		.CompiledMethodLoad = CompiledMethodLoad,
		.DynamicCodeGenerated = DynamicCodeGenerated,
		.VMDeath = VMDeath,
	};

	jvmtiCapabilities mycapabilities = {
		.can_generate_compiled_method_load_events = 1,
		.can_get_line_numbers = 1,
		.can_get_source_file_name = 1
	};

	jint eres = (*vm)->GetEnv(vm, (void **) &jvmti, JVMTI_VERSION_1_0);
	if (eres) {
		fprintf(stderr, "Unable to hook into JVMTI, error code = 0x%x\n", eres);
		return eres;
	}

	CHECKBADRET((*jvmti)->SetEventCallbacks(jvmti, &mycallbacks, sizeof(mycallbacks)), -1);
	CHECKBADRET((*jvmti)->AddCapabilities(jvmti, &mycapabilities), -2);
	CHECKBADRET((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_COMPILED_METHOD_LOAD, NULL), -3);
	CHECKBADRET((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_DYNAMIC_CODE_GENERATED, NULL), -4);
	CHECKBADRET((*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH, NULL), -5);

	pid = getpid();
	ret = asprintf(&dirname, "/tmp/perf-%d.map.d/", pid);

	if (ret == -1) {
		fprintf(stderr, "Couldn't allocate string %s\n",
			strerror(errno));
		return -6;
	}

	md = mkdir(dirname, 0700);
	if (md) {
		fprintf(stderr, "Unable to create directory %s - %s\n", dirname, strerror(errno));
		return -7;
	}

	free(dirname);
	fprintf(stderr, "native-java-agent v0.01 tracking codegen...\n");
	return 0;
}

/*
 * We are being unloaded from the JVM, dump out perf map
 * and free everything up.
 */
JNIEXPORT void JNICALL
Agent_OnUnload(JavaVM *vm)
{
	me_outputperfmap();
	me_freemethodentries();
}
