#include "mem.h"
#include "generic_log.h"

#define LOCK(mgr) do { if ((mgr)->thread_safe) (mgr)->lock(); } while(0)
#define UNLOCK(mgr) do { if ((mgr)->thread_safe) (mgr)->unlock(); } while(0)

#define LOG_MAX_LINE 256  // Can be adjusted according to actual needs
#define LOG_FILE_FREE_MARGIN (8u * 1024u)  /* min free bytes to keep file logging enabled */

static log_manager_t *log_manager = NULL;

static const char* level_strings[] = {
    "DEBUG", "INFO", "WARNING", "ERROR", "FATAL"
};

// Strip ANSI escape sequences (e.g., color codes) from src into dst
static int strip_ansi_sequences(const char *src, char *dst, size_t dst_size)
{
    if (!src || !dst || dst_size == 0) return 0;
    size_t si = 0, di = 0;
    while (src[si] != '\0' && di + 1 < dst_size) {
        if (src[si] == '\x1b' && src[si + 1] == '[') {
            // Skip until 'm' or string end
            si += 2;
            while (src[si] != '\0' && src[si] != 'm') {
                si++;
            }
            if (src[si] == 'm') {
                si++; // skip 'm'
            }
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return (int)di;
}

/* Returns 0 on success, -1 if the final (current-file) rename failed — the
 * only rotation step whose failure reliably indicates a broken FS. Intermediate
 * renames/removes are best-effort: the .N slots may not exist on the first few
 * rotations, so their failure is benign and ignored. */
static int rotate_file(const char *filename, int max_files)
{
    if (!log_manager || !log_manager->file_ops) return -1;
    log_file_ops_t *ops = log_manager->file_ops;

    if (max_files <= 0) return 0;

    char old_path[256];
    char new_path[256];

    /* Drop the oldest slot if present (may not exist yet — benign). */
    snprintf(old_path, sizeof(old_path), "%s.%d", filename, max_files);
    (void)ops->remove(old_path);

    /* Shift .i -> .i+1 from the top down. Tolerate absence on early rotations. */
    for (int i = max_files - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", filename, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", filename, i + 1);
        (void)ops->rename(old_path, new_path);
    }

    /* The current file exists (rotation only triggers after append exceeded
     * max_size), so this rename must succeed — failure means the FS is broken. */
    snprintf(new_path, sizeof(new_path), "%s.1", filename);
    return (ops->rename(filename, new_path) == 0) ? 0 : -1;
}

int log_register_module(const char *name, LogLevel level, LogLevel file_level)
{
    if (!log_manager) return -1;

    LOCK(log_manager);
    
    // Check if module exists
    for (size_t i = 0; i < log_manager->module_count; i++) {
        if (strcmp(log_manager->modules[i].name, name) == 0) {
            log_manager->modules[i].level = level;
            log_manager->modules[i].file_level = file_level;
            UNLOCK(log_manager);
            return 0;
        }
    }

    // Add new module
    log_module_t *new_modules = realloc(log_manager->modules, 
        (log_manager->module_count + 1) * sizeof(log_module_t));
    if (!new_modules) {
        UNLOCK(log_manager);
        return -1;
    }

    log_manager->modules = new_modules;
    char *name_copy = strdup(name);
    if (!name_copy) {
        UNLOCK(log_manager);
        return -1;
    }
    log_manager->modules[log_manager->module_count].name = name_copy;
    log_manager->modules[log_manager->module_count].level = level;
    log_manager->modules[log_manager->module_count].file_level = file_level;
    log_manager->module_count++;
    
    UNLOCK(log_manager);
    return 0;
}

int log_add_custom_output(log_custom_output_func_t func) 
{
    if (!log_manager || !func) return -1;

    LOCK(log_manager);

    log_custom_output_node_t *node = log_manager->custom_outputs;
    while (node) {
        if (node->func == func) {
            UNLOCK(log_manager);
            return 1;
        }
        node = node->next;
    }

    node = hal_mem_alloc_fast(sizeof(log_custom_output_node_t));
    if (!node) {
        UNLOCK(log_manager);
        return -1;
    }
    node->func = func;
    node->next = log_manager->custom_outputs;
    log_manager->custom_outputs = node;
    UNLOCK(log_manager);
    return 0;
}

int log_remove_custom_output(log_custom_output_func_t func) 
{
    if (!log_manager || !func) return -1;

    LOCK(log_manager);
    log_custom_output_node_t **cur = &log_manager->custom_outputs;
    while (*cur) {
        if ((*cur)->func == func) {
            log_custom_output_node_t *to_delete = *cur;
            *cur = to_delete->next;
            hal_mem_free(to_delete);
            UNLOCK(log_manager);
            return 0;
        }
        cur = &((*cur)->next);
    }
    UNLOCK(log_manager);
    return -1;
}

int log_add_output(OutputType type, const char *filename, size_t max_size, int max_files)
{
    if (!log_manager) return -1;

    LOCK(log_manager);

    log_output_t *new_outputs = realloc(log_manager->outputs,
        (log_manager->output_count + 1) * sizeof(log_output_t));
    if (!new_outputs) {
        UNLOCK(log_manager);
        return -1;
    }

    log_manager->outputs = new_outputs;
    log_output_t *output = &log_manager->outputs[log_manager->output_count];
    memset(output, 0, sizeof(log_output_t));
    
    output->type = type;
    output->enabled = true;
    if (type == OUTPUT_FILE) {
        if (!filename) {
            UNLOCK(log_manager);
            return -1;
        }
        char *fname = strdup(filename);
        if (!fname) {
            UNLOCK(log_manager);
            return -1;
        }
        output->config.file.filename = fname;
        output->config.file.max_size = max_size;
        output->config.file.max_files = max_files;

        /* One-shot free-space check at registration: if the FS reports low
         * space, hard-disable this output for the run so the hot log_message
         * path never attempts a write. Kept here (not in log_message) so the
         * per-line log cost stays a single bool check. If the FS is not ready
         * yet (rc != 0), leave it enabled — the circuit breaker in log_message
         * will disable on the first failed write. */
        if (log_manager->file_ops && log_manager->file_ops->get_free_bytes) {
            uint64_t free_bytes = 0;
            if (log_manager->file_ops->get_free_bytes(&free_bytes) == 0) {
                uint64_t threshold = (uint64_t)max_size + LOG_FILE_FREE_MARGIN;
                if (free_bytes < threshold) {
                    output->config.file.disabled = true;
                    fprintf(stderr, "[LOG] file logging disabled: low free space at registration (%lu B, need %lu B)\r\n",
                            (unsigned long)free_bytes, (unsigned long)threshold);
                }
            }
        }
    }
    
    log_manager->output_count++;
    UNLOCK(log_manager);
    return 0;
}

int log_set_output_enabled(OutputType type, bool enabled)
{
    if (!log_manager) return -1;

    LOCK(log_manager);
    int changed = 0;
    for (size_t i = 0; i < log_manager->output_count; i++) {
        log_output_t *output = &log_manager->outputs[i];
        if (output->type == type) {
            output->enabled = enabled;
            changed++;
        }
    }
    UNLOCK(log_manager);
    return changed; // Return number of modified items
}

void log_message(LogLevel level, const char *module_name, const char *format, ...)
{
    if (!log_manager) return;

    LOCK(log_manager);

    char log_buffer[LOG_MAX_LINE];
    char msg_buffer[LOG_MAX_LINE];
    LogLevel module_level = LOG_INFO;
    LogLevel file_level = LOG_INFO;
    char timestamp[20] = {0};
    int found = 0;
    for (size_t i = 0; i < log_manager->module_count; i++) {
        if (strcmp(log_manager->modules[i].name, module_name) == 0) {
            module_level = log_manager->modules[i].level;
            file_level = log_manager->modules[i].file_level;
            found = 1;
            break;
        }
    }
    if (!found || (level < module_level && level < file_level)) {
        UNLOCK(log_manager);
        return;
    }

    // Format timestamp
    if (log_manager->get_time_func != NULL) {
        time_t now = log_manager->get_time_func();
        struct tm *tm_info = localtime(&now);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);
    }

    // Format message body
    va_list args;
    va_start(args, format);
    int msg_len = vsnprintf(msg_buffer, LOG_MAX_LINE, format, args);
    va_end(args);
    if (msg_len < 0) msg_len = 0;
    if (msg_len >= LOG_MAX_LINE - 2) msg_len = LOG_MAX_LINE - 2;
    msg_buffer[msg_len] = '\r';
    msg_buffer[msg_len + 1] = '\n';
    msg_buffer[msg_len + 2] = '\0';

    // Format complete log line
    int log_line_len = 0;
    if (level == LOG_SIMPLE) {
        // Simple log uses message content directly
        log_line_len = snprintf(log_buffer, LOG_MAX_LINE, "%s", msg_buffer);
    } else {
        log_line_len = snprintf(log_buffer, LOG_MAX_LINE,
            "[%s] [%s] [%s] %s",
            timestamp, module_name, level_strings[level], msg_buffer);
    }
    if (log_line_len < 0) log_line_len = 0;
    if (log_line_len >= LOG_MAX_LINE) log_line_len = LOG_MAX_LINE - 1;
    log_buffer[log_line_len] = '\0';

    // Write to output
    for (size_t i = 0; i < log_manager->output_count; i++) {
        log_output_t *output = &log_manager->outputs[i];
        if (!output->enabled) continue; 
    
        switch (output->type) {
            case OUTPUT_CONSOLE:
                if (level < module_level) break;
                fprintf(stdout, "%.*s", log_line_len, log_buffer);
                fflush(stdout);
                break;

            case OUTPUT_FILE: {
                if (level < file_level || level == LOG_SIMPLE) break;
                /* Circuit breaker: once a prior write/open/rename failed (or
                 * registration found free space too low), skip all file output
                 * for the rest of this run — stops the "Failed to write log
                 * file" retry storm on a full or broken FS. Free space is
                 * checked once at registration (log_add_output), not here, so
                 * the per-line cost is just this single bool. Not re-enabled
                 * by log_set_output_enabled. */
                if (output->config.file.disabled) break;
                log_file_ops_t *ops = log_manager->file_ops;
                const char *filename = output->config.file.filename;

                char clean_buffer[LOG_MAX_LINE];
                int clean_len = strip_ansi_sequences(log_buffer, clean_buffer, sizeof(clean_buffer));
                if (clean_len < 0) clean_len = 0;

                size_t current_size = 0;
                struct stat st;
                if (ops->fstat && ops->fstat(filename, &st) == 0) {
                    current_size = st.st_size;
                }
                size_t new_size = current_size + (size_t)clean_len;
                if (output->config.file.max_size > 0 && new_size > output->config.file.max_size) {
                    if (rotate_file(filename, output->config.file.max_files) != 0) {
                        output->config.file.disabled = true;
                        fprintf(stderr, "[LOG] file logging disabled: rotation failed\r\n");
                        break;
                    }
                }
                void *file = ops->fopen(filename, "a");
                if (!file) {
                    output->config.file.disabled = true;
                    fprintf(stderr, "[LOG] file logging disabled: open failed\r\n");
                    break;
                }
                size_t written = ops->fwrite(file, clean_buffer, (size_t)clean_len);
                if (written != (size_t)clean_len) {
                    ops->fclose(file);
                    output->config.file.disabled = true;
                    fprintf(stderr, "[LOG] file logging disabled: write failed\r\n");
                    break;
                }
                ops->fflush(file);
                ops->fclose(file);
                break;
            }
            case OUTPUT_CUSTOM:
                if (level < module_level) break;
                log_custom_output_node_t *node = log_manager->custom_outputs;
                while (node) {
                    if (node->func) {
                        node->func(log_buffer, log_line_len);
                    }
                    node = node->next;
                }
                break;
        }
    }

    UNLOCK(log_manager);
}


void log_shutdown()
{
    if (!log_manager) return;

    LOCK(log_manager);
    
    // Free modules
    for (size_t i = 0; i < log_manager->module_count; i++) {
        hal_mem_free((void*)log_manager->modules[i].name);
    }
    hal_mem_free(log_manager->modules);

    // Free outputs
    for (size_t i = 0; i < log_manager->output_count; i++) {
        log_output_t *output = &log_manager->outputs[i];
        if (output->type == OUTPUT_FILE) {
            hal_mem_free(output->config.file.filename);
        }
    }
    hal_mem_free(log_manager->outputs);

    UNLOCK(log_manager);

    log_manager = NULL;
}

int log_init(log_manager_t *mgr, log_lock_func_t lock, log_unlock_func_t unlock, log_file_ops_t *file_ops, log_get_time_func_t get_time_func)
{
    if (mgr == NULL) return -1;

    log_manager = mgr;

    log_manager->modules = NULL;
    log_manager->module_count = 0;
    log_manager->outputs = NULL;
    log_manager->output_count = 0;
    log_manager->file_ops = file_ops;
    log_manager->get_time_func = get_time_func;
    if (lock && unlock) {
        mgr->lock = lock;
        mgr->unlock = unlock;
        mgr->thread_safe = true;
    } else {
        mgr->thread_safe = false;
    }
    return 0;
}

#if 0
int main() {
    log_init();
    log_register_module("NETWORK", LOG_INFO);
    log_register_module("DATABASE", LOG_WARNING);
    
    log_add_output(OUTPUT_CONSOLE, NULL, 0, 0);
    log_add_output(OUTPUT_FILE, "app.log", 1024, 3);

    log_message(LOG_INFO, "NETWORK", "Connection established");
    log_message(LOG_DEBUG, "NETWORK", "This debug message won't show");
    log_message(LOG_ERROR, "DATABASE", "Connection timeout");

    log_shutdown();
    return 0;
}
#endif