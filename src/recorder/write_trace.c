#include <assert.h>
#include <errno.h>
#include <sched.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#include "../share/sys.h"
#include "../share/types.h"
#include "../share/ipc.h"
#include "../share/trace.h"
#include "../share/hpc.h"
#include "../share/config.h"

#include "write_trace.h"

static FILE* syscall_header;
static FILE* raw_data;
static FILE* trace_file;

static uint32_t thread_time[100000];
static uint32_t global_time = 0;
static char* trace_path;

#define BUF_SIZE 1024;
#define LINE_SIZE 50;

void write_open_inst_dump(struct context* context)
{
	char path[64];
	char tmp[32];
	strcpy(path, trace_path);
	sprintf(tmp, "/inst_dump_%d",context->child_tid);
	strcat(path, tmp);
	context->inst_dump = sys_fopen(path, "a+");
}

static unsigned int get_global_time_incr()
{
	return global_time++;
}

static unsigned int get_time_incr(pid_t tid)
{
	return thread_time[tid]++;
}

unsigned int get_time(pid_t tid)
{
	return thread_time[tid];
}

/**
 * sets up the directory where all trace files will be stored. If the trace
 * file already exists, the version number is increased
 */
void setup_trace_dir(int version)
{
	char ver[32], path[64];
	/* convert int to char* */
	sprintf(ver, "_%d", version);
	strcpy(path, "./trace");

	const char* tmp_path = strcat(path, ver);

	if (mkdir(tmp_path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1) {
		int error = errno;
		if (error == EEXIST) {
			setup_trace_dir(version + 1);
		}
	} else {
		trace_path = sys_malloc(strlen(tmp_path) + 1);
		strcpy(trace_path, tmp_path);
	}
}

void record_argv_envp(int __argc, char* argv[], char* envp[])
{

	/* -2 because
	 *  argv[0] = name of RecReplay
	 *  argv[1] = --record
	 *  argv[2] = name of executable
	 *  argv[3] = arg1
	 *  argv[4] = arg2
	 *  ...record
	 */
	int argc = __argc - 2;

	char tmp[128], path[64];
	int i, j;
	/* construct path to file */
	strcpy(path, trace_path);
	strcpy(tmp, "/arg_env");
	strcat(path, tmp);

	FILE* arg_env = (FILE*) sys_fopen(path, "a+");

	/* print argc */
	fprintf(arg_env, "%d\n", argc);

	/* print arguments to file */
	for (i = 0; i < argc; i++) {
		fprintf(arg_env, "%s\n", argv[i]);
	}

	/* figure out the length of envp */
	i = 0;
	while (envp[i] != NULL) {
		i++;
	}
	fprintf(arg_env, "%d\n", i);

	for (j = 0; j < i; j++) {
		fprintf(arg_env, "%s\n", envp[j]);
	}
	sys_fclose(arg_env);
}

void open_trace_files(void)
{
	char tmp[128], path[64];

	strcpy(path, trace_path);
	strcpy(tmp, "/trace");
	strcat(path, tmp);

	trace_file = sys_fopen(path, "a+");

	strcpy(path, trace_path);
	strcpy(tmp, "/syscall_input");
	strcat(path, tmp);
	syscall_header = sys_fopen(path, "a+");

	strcpy(path, trace_path);
	strcpy(tmp, "/raw_data");
	strcat(path, tmp);
	raw_data = sys_fopen(path, "a+");
}

void init_trace_files(void)
{
	/* init trace file */
	fprintf(trace_file, "%11s", "global_time");
	fprintf(trace_file, "%11s", "thread_time");
	fprintf(trace_file, "%11s", "tid");
	fprintf(trace_file, "%11s", "reason");
	fprintf(trace_file, "%11s", "entry/exit");
	fprintf(trace_file, "%20s", "hw_interrupts");
	fprintf(trace_file, "%20s", "page_faults");
	fprintf(trace_file, "%20s", "adapted_rbc");

	fprintf(trace_file, "%11s", "eax");
	fprintf(trace_file, "%11s", "ebx");
	fprintf(trace_file, "%11s", "ecx");
	fprintf(trace_file, "%11s", "edx");
	fprintf(trace_file, "%11s", "esi");
	fprintf(trace_file, "%11s", "edi");
	fprintf(trace_file, "%11s", "ebp");
	fprintf(trace_file, "%11s", "orig_eax");
	fprintf(trace_file, "%11s", "eip");
	fprintf(trace_file, "%11s", "eflags");
	fprintf(trace_file, "\n");

	/* print human readable header */
	fprintf(syscall_header, "%11s", "time");
	fprintf(syscall_header, "%11s", "syscall");
	fprintf(syscall_header, "%11s", "addr");
	fprintf(syscall_header, "%11s\n", "size");

}

void close_trace_files()
{
	sys_fclose(syscall_header);
	sys_fclose(raw_data);
	sys_fclose(trace_file);
}

static void print_trace_meta_information(pid_t tid, int stop_reason, pid_t new_tid)
{
	fprintf(trace_file, "%11d", get_global_time_incr());
	fprintf(trace_file, "%11u", get_time_incr(tid));
	fprintf(trace_file, "%11d", tid);
	fprintf(trace_file, "%11d", stop_reason);
	fprintf(trace_file, "%11d", new_tid);
}

static void record_performance_data(struct context *ctx)
{
	fprintf(trace_file, "%20llu", read_hw_int(ctx->hpc));
	fprintf(trace_file, "%20llu", read_page_faults(ctx->hpc));
	fprintf(trace_file, "%20llu", read_rbc_up(ctx->hpc));
}
static void record_register_file(pid_t tid, FILE* file)
{
	struct user_regs_struct regs;
	read_child_registers(tid, &regs);

	fprintf(file, "%11lu", regs.eax);
	fprintf(file, "%11lu", regs.ebx);
	fprintf(file, "%11lu", regs.ecx);
	fprintf(file, "%11lu", regs.edx);
	fprintf(file, "%11lu", regs.esi);
	fprintf(file, "%11lu", regs.edi);
	fprintf(file, "%11lu", regs.ebp);
	fprintf(file, "%11lu", regs.orig_eax);
	fprintf(file, "%11lu", regs.eip);
	fprintf(file, "%11lu", regs.eflags);
	fprintf(file, "\n");
}

void record_event(struct context *ctx, int entry)
{
	print_trace_meta_information(ctx->child_tid, ctx->event, entry);

	/* we record a system call */
	if (ctx->event != 0) {
		record_performance_data(ctx);
		record_register_file(ctx->child_tid, trace_file);
		/* reset the performance counters */
		reset_hpc(ctx,MAX_RECORD_INTERVAL);
	} else {
		fprintf(trace_file, "\n");
	}
}

static void print_header(int syscall, uint32_t addr)
{
	fprintf(syscall_header, "%11u", global_time);
	fprintf(syscall_header, "%11d", syscall);
	fprintf(syscall_header, "%11u", addr);
}

/**
 * Writes data into the raw_data file and generates a corresponding entry in
 * syscall_input.
 */
void record_child_data_tid(pid_t tid, int syscall, size_t len, long int child_ptr)
{
	print_header(syscall, child_ptr);
	fprintf(syscall_header, "%11d\n", len);

	if (child_ptr != 0) {
		void* buf = read_child_data_tid(tid, len, child_ptr);
		/* ensure that everything is written */
		assert(fwrite(buf, 1, len, raw_data) == len);
		sys_free((void**) &buf);
	}
}


/**
 * Writes data into the raw_data file and generates a corresponding entry in
 * syscall_input.
 */
void record_child_data(struct context *ctx, int syscall, size_t len, long int child_ptr)
{
	print_header(syscall, child_ptr);
	fprintf(syscall_header, "%11d\n", len);

	if (child_ptr != 0) {
		void* buf = read_child_data(ctx, len, child_ptr);
		/* ensure that everything is written */
		assert(fwrite(buf, 1, len, raw_data) == len);
		sys_free((void**) &buf);
	}
}


void record_child_str(pid_t tid, int syscall, long int child_ptr)
{
	print_header(syscall, child_ptr);
	char* buf = read_child_str(tid, child_ptr);
	size_t len = strlen(buf) + 1;
	fprintf(syscall_header, "%11d\n", len);
	int bytes_written = fwrite(buf, 1, len, raw_data);

	assert (bytes_written == len);
	sys_free((void**) &buf);
}

void record_inst(struct context* context, char* inst)
{
	fprintf(context->inst_dump, "%d:%-40s\n", context->child_tid, inst);
	record_register_file(context->child_tid, context->inst_dump);
}

void record_inst_done(struct context* context)
{
	fprintf(context->inst_dump, "%s\n", "__done__");
}
