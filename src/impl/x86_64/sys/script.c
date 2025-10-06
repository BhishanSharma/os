#include "script.h"
#include "print.h"
#include "fat32.h"
#include "heap.h"
#include "string.h"
#include "shell.h"

#define MAX_SCRIPT_SIZE 4096
#define MAX_LINE_LENGTH 256
#define MAX_VARIABLES 16
#define MAX_VAR_NAME 32
#define MAX_VAR_VALUE 128

typedef struct {
    char name[MAX_VAR_NAME];
    char value[MAX_VAR_VALUE];
} variable_t;

static variable_t variables[MAX_VARIABLES];
static int var_count = 0;

// Forward declarations
static int execute_line(const char* line);
static void set_variable(const char* name, const char* value);
static const char* get_variable(const char* name);
static void expand_variables(char* line, char* output, int max_len);

// Set a variable
static void set_variable(const char* name, const char* value) {
    // Check if variable exists
    for (int i = 0; i < var_count; i++) {
        if (strcmp(variables[i].name, name) == 0) {
            // Update existing
            int j = 0;
            while (value[j] && j < MAX_VAR_VALUE - 1) {
                variables[i].value[j] = value[j];
                j++;
            }
            variables[i].value[j] = '\0';
            return;
        }
    }
    
    // Add new variable
    if (var_count < MAX_VARIABLES) {
        int i = 0;
        while (name[i] && i < MAX_VAR_NAME - 1) {
            variables[var_count].name[i] = name[i];
            i++;
        }
        variables[var_count].name[i] = '\0';
        
        i = 0;
        while (value[i] && i < MAX_VAR_VALUE - 1) {
            variables[var_count].value[i] = value[i];
            i++;
        }
        variables[var_count].value[i] = '\0';
        
        var_count++;
    }
}

// Get variable value
static const char* get_variable(const char* name) {
    for (int i = 0; i < var_count; i++) {
        if (strcmp(variables[i].name, name) == 0) {
            return variables[i].value;
        }
    }
    return "";
}

// Expand $VARIABLE in line
static void expand_variables(char* line, char* output, int max_len) {
    int out_pos = 0;
    int i = 0;
    
    while (line[i] && out_pos < max_len - 1) {
        if (line[i] == '$') {
            // Extract variable name
            char var_name[MAX_VAR_NAME];
            int var_pos = 0;
            i++;
            
            while (line[i] && (
                (line[i] >= 'A' && line[i] <= 'Z') ||
                (line[i] >= 'a' && line[i] <= 'z') ||
                (line[i] >= '0' && line[i] <= '9') ||
                line[i] == '_') && var_pos < MAX_VAR_NAME - 1) {
                var_name[var_pos++] = line[i++];
            }
            var_name[var_pos] = '\0';
            
            // Get and insert value
            const char* value = get_variable(var_name);
            int j = 0;
            while (value[j] && out_pos < max_len - 1) {
                output[out_pos++] = value[j++];
            }
        } else {
            output[out_pos++] = line[i++];
        }
    }
    output[out_pos] = '\0';
}

static int execute_line(const char* line) {
    char expanded[MAX_LINE_LENGTH];
    char trimmed[MAX_LINE_LENGTH];
    
    // Skip empty lines and comments
    if (line[0] == '\0' || line[0] == '#') {
        return 0;
    }
    
    // Trim leading spaces
    int start = 0;
    while (line[start] == ' ' || line[start] == '\t') start++;
    
    int i = 0;
    while (line[start + i]) {
        trimmed[i] = line[start + i];
        i++;
    }
    trimmed[i] = '\0';
    
    if (trimmed[0] == '\0') return 0;
    
    // Expand variables
    expand_variables(trimmed, expanded, MAX_LINE_LENGTH);
    
    // Check for variable assignment (VAR=value)
    char* equals = expanded;
    while (*equals && *equals != '=') equals++;
    
    if (*equals == '=') {
        *equals = '\0';
        set_variable(expanded, equals + 1);
        return 0;
    }
    
    // Handle special commands
    if (strncmp(expanded, "echo ", 5) == 0) {
        print_str(expanded + 5);
        print_str("\n");
        return 0;
    }
    
    if (strcmp(expanded, "exit") == 0) {
        return -1; // Signal to exit script
    }
    
    if (strncmp(expanded, "sleep ", 6) == 0) {
        // Parse number
        int seconds = 0;
        const char* num = expanded + 6;
        while (*num >= '0' && *num <= '9') {
            seconds = seconds * 10 + (*num - '0');
            num++;
        }
        extern void sleep(uint32_t ms);
        sleep(seconds * 1000);
        return 0;
    }
    
    // Execute as shell command
    return shell_execute_command(expanded);
}

// Run a script file
int script_run(const char* filename) {
    if (!fat32_file_exists(filename)) {
        print_error("Script not found");
        return -1;
    }
    
    uint32_t size = fat32_get_file_size(filename);
    if (size == 0) {
        print_warning("Empty script");
        return 0;
    }
    
    if (size > MAX_SCRIPT_SIZE) {
        print_error("Script too large");
        return -1;
    }
    
    uint8_t* script_data = kmalloc(size + 1);
    if (!script_data) {
        print_error("Out of memory");
        return -1;
    }
    
    int bytes = fat32_read_file(filename, script_data, size);
    if (bytes < 0) {
        print_error("Failed to read script");
        kfree(script_data);
        return -1;
    }
    
    script_data[bytes] = '\0';
    
    // Reset variables
    var_count = 0;
    
    // Parse and execute line by line
    char line[MAX_LINE_LENGTH];
    int line_pos = 0;
    int line_num = 1;
    
    for (int i = 0; i <= bytes; i++) {
        if (script_data[i] == '\n' || script_data[i] == '\0') {
            line[line_pos] = '\0';
            
            int result = execute_line(line);
            if (result == -1) {
                // Exit command
                break;
            }
            
            line_pos = 0;
            line_num++;
        } else if (line_pos < MAX_LINE_LENGTH - 1) {
            line[line_pos++] = script_data[i];
        }
    }
    
    kfree(script_data);
    return 0;
}
