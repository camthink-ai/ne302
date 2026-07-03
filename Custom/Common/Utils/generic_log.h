#ifndef GENERIC_LOG_H
#define GENERIC_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

typedef void (*log_lock_func_t)(void);
typedef void (*log_unlock_func_t)(void);
typedef uint64_t (*log_get_time_func_t)(void);

typedef void* (*log_fopen_func_t)(const char *filename, const char *mode);
typedef int   (*log_fclose_func_t)(void *handle);
typedef int   (*log_remove_func_t)(const char *filename);
typedef int   (*log_rename_func_t)(const char *oldname, const char *newname);
typedef long  (*log_ftell_func_t)(void *handle);
typedef int   (*log_fseek_func_t)(void *handle, long offset, int whence);
typedef int   (*log_fflush_func_t)(void *handle);
typedef int   (*log_fwrite_func_t)(void *handle, const void *buf, size_t size);
typedef int   (*log_stat_func_t)(const char *filename, struct stat *st);
typedef int   (*log_get_free_bytes_func_t)(uint64_t *out_free_bytes);  /* 0 = ok, -1 = FS not ready */
typedef void (*log_custom_output_func_t)(const char *msg, int len);

typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL,
    LOG_SIMPLE,
    LOG_LEVEL_COUNT
} LogLevel;

typedef struct log_custom_output_node {
    log_custom_output_func_t func;
    struct log_custom_output_node *next;
} log_custom_output_node_t;

typedef struct {
    log_fopen_func_t   fopen;
    log_fclose_func_t  fclose;
    log_remove_func_t  remove;
    log_rename_func_t  rename;
    log_ftell_func_t   ftell;
    log_fseek_func_t   fseek;
    log_fflush_func_t  fflush;
    log_fwrite_func_t  fwrite;
    log_stat_func_t    fstat;
    log_get_free_bytes_func_t get_free_bytes;  /* optional; NULL ⇒ skip free-space pre-check */
} log_file_ops_t;

typedef struct {
    const char *name;
    LogLevel level;
    LogLevel file_level;
} log_module_t;

typedef enum {
    OUTPUT_CONSOLE,
    OUTPUT_FILE,
    OUTPUT_CUSTOM
} OutputType;

typedef struct {
    OutputType type;
    union {
        struct {
            char *filename;
            size_t max_size;
            int max_files;
            bool disabled;        /* hard-disabled for this run — set at registration (low free
                                   * space) or by the circuit breaker on a write/open/rename
                                   * failure. Not re-enabled by log_set_output_enabled. */
        } file;
        struct {
            log_custom_output_func_t func;
        } custom;
    } config;
    bool enabled;
} log_output_t;

typedef struct {
    log_module_t *modules;
    size_t module_count;
    log_output_t *outputs;
    size_t output_count;
    log_file_ops_t *file_ops;
    log_lock_func_t lock;
    log_unlock_func_t unlock;
    log_get_time_func_t get_time_func;
    log_custom_output_node_t *custom_outputs;
    bool thread_safe;
} log_manager_t;

int log_register_module(const char *name, LogLevel level, LogLevel file_level);
int log_add_custom_output(log_custom_output_func_t func);
int log_remove_custom_output(log_custom_output_func_t func);
int log_add_output(OutputType type, const char *filename, size_t max_size, int max_files);
int log_set_output_enabled(OutputType type, bool enabled);
void log_message(LogLevel level, const char *module_name, const char *format, ...);
int log_init(log_manager_t *mgr, log_lock_func_t lock, log_unlock_func_t unlock, log_file_ops_t *file_ops, log_get_time_func_t get_time_func);
#endif