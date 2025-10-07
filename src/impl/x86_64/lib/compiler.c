// compiler.c - A simple C compiler for your OS
#include "lib/compiler.h"
#include "lib/print.h"
#include "lib/string.h"
#include "drivers/heap.h"
#include "drivers/fat32.h"

// ===== TOKENIZER =====

typedef enum {
    TOK_EOF,
    TOK_INT,
    TOK_CHAR,
    TOK_VOID,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_RETURN,
    TOK_IDENTIFIER,
    TOK_NUMBER,
    TOK_STRING,
    TOK_LPAREN,      // (
    TOK_RPAREN,      // )
    TOK_LBRACE,      // {
    TOK_RBRACE,      // }
    TOK_SEMICOLON,   // ;
    TOK_COMMA,       // ,
    TOK_ASSIGN,      // =
    TOK_PLUS,        // +
    TOK_MINUS,       // -
    TOK_STAR,        // *
    TOK_SLASH,       // /
    TOK_LT,          // <
    TOK_GT,          // >
    TOK_EQ,          // ==
    TOK_NEQ,         // !=
    TOK_LEQ,         // <=
    TOK_GEQ,         // >=
} TokenType;

typedef struct {
    TokenType type;
    char* value;
    int line;
    int col;
} Token;

typedef struct {
    char* source;
    int pos;
    int line;
    int col;
    Token* tokens;
    int token_count;
    int token_capacity;
} Lexer;

// ===== PARSER / AST =====

typedef enum {
    NODE_PROGRAM,
    NODE_FUNCTION,
    NODE_VAR_DECL,
    NODE_RETURN,
    NODE_IF,
    NODE_WHILE,
    NODE_BLOCK,
    NODE_CALL,
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_NUMBER,
    NODE_IDENTIFIER,
    NODE_STRING,
    NODE_ASSIGN,
} NodeType;

typedef struct ASTNode {
    NodeType type;
    char* value;
    struct ASTNode** children;
    int child_count;
    int child_capacity;
} ASTNode;

// ===== CODE GENERATOR =====

typedef enum {
    OP_PUSH,      // Push value to stack
    OP_POP,       // Pop from stack
    OP_ADD,       // Add top two stack values
    OP_SUB,       // Subtract
    OP_MUL,       // Multiply
    OP_DIV,       // Divide
    OP_LOAD,      // Load variable
    OP_STORE,     // Store variable
    OP_CALL,      // Call function
    OP_RET,       // Return from function
    OP_JMP,       // Unconditional jump
    OP_JZ,        // Jump if zero
    OP_JNZ,       // Jump if not zero
    OP_CMP_LT,    // Compare less than
    OP_CMP_GT,    // Compare greater than
    OP_CMP_EQ,    // Compare equal
    OP_SYSCALL,   // System call (for printf, etc.)
    OP_HALT,      // Stop execution
} OpCode;

typedef struct {
    OpCode op;
    int operand;
    char* label;
} Instruction;

typedef struct {
    Instruction* instructions;
    int count;
    int capacity;
    
    // Symbol table for variables
    char** variables;
    int var_count;
    int var_capacity;
    
    // String literals
    char** strings;
    int string_count;
    int string_capacity;
} CodeGen;

// ===== VIRTUAL MACHINE =====

typedef struct {
    int* stack;
    int sp;           // Stack pointer
    int* locals;      // Local variables
    int local_count;
    
    Instruction* code;
    int ip;           // Instruction pointer
    int code_size;
    
    char** strings;   // String literals
    int string_count;
    
    int running;
} VM;

// ===== LEXER IMPLEMENTATION =====

static void lexer_init(Lexer* lex, char* source) {
    lex->source = source;
    lex->pos = 0;
    lex->line = 1;
    lex->col = 1;
    lex->token_count = 0;
    lex->token_capacity = 64;
    lex->tokens = (Token*)kmalloc(sizeof(Token) * lex->token_capacity);
}

static void add_token(Lexer* lex, TokenType type, char* value) {
    if (lex->token_count >= lex->token_capacity) {
        lex->token_capacity *= 2;
        Token* new_tokens = (Token*)kmalloc(sizeof(Token) * lex->token_capacity);
        for (int i = 0; i < lex->token_count; i++) {
            new_tokens[i] = lex->tokens[i];
        }
        kfree(lex->tokens);
        lex->tokens = new_tokens;
    }
    
    lex->tokens[lex->token_count].type = type;
    lex->tokens[lex->token_count].value = value;
    lex->tokens[lex->token_count].line = lex->line;
    lex->tokens[lex->token_count].col = lex->col;
    lex->token_count++;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static void skip_whitespace(Lexer* lex) {
    while (lex->source[lex->pos]) {
        char c = lex->source[lex->pos];
        if (c == ' ' || c == '\t' || c == '\r') {
            lex->pos++;
            lex->col++;
        } else if (c == '\n') {
            lex->pos++;
            lex->line++;
            lex->col = 1;
        } else if (c == '/' && lex->source[lex->pos + 1] == '/') {
            // Skip line comment
            while (lex->source[lex->pos] && lex->source[lex->pos] != '\n') {
                lex->pos++;
            }
        } else {
            break;
        }
    }
}

static char* extract_identifier(Lexer* lex) {
    int start = lex->pos;
    while (is_alnum(lex->source[lex->pos])) {
        lex->pos++;
        lex->col++;
    }
    
    int len = lex->pos - start;
    char* id = (char*)kmalloc(len + 1);
    for (int i = 0; i < len; i++) {
        id[i] = lex->source[start + i];
    }
    id[len] = '\0';
    return id;
}

static char* extract_number(Lexer* lex) {
    int start = lex->pos;
    while (is_digit(lex->source[lex->pos])) {
        lex->pos++;
        lex->col++;
    }
    
    int len = lex->pos - start;
    char* num = (char*)kmalloc(len + 1);
    for (int i = 0; i < len; i++) {
        num[i] = lex->source[start + i];
    }
    num[len] = '\0';
    return num;
}

static char* extract_string(Lexer* lex) {
    lex->pos++; // Skip opening quote
    lex->col++;
    
    int start = lex->pos;
    while (lex->source[lex->pos] && lex->source[lex->pos] != '"') {
        if (lex->source[lex->pos] == '\\') {
            lex->pos += 2; // Skip escape sequence
            lex->col += 2;
        } else {
            lex->pos++;
            lex->col++;
        }
    }
    
    int len = lex->pos - start;
    char* str = (char*)kmalloc(len + 1);
    int j = 0;
    for (int i = start; i < lex->pos; i++) {
        if (lex->source[i] == '\\' && lex->source[i+1] == 'n') {
            str[j++] = '\n';
            i++;
        } else if (lex->source[i] == '\\' && lex->source[i+1] == 't') {
            str[j++] = '\t';
            i++;
        } else {
            str[j++] = lex->source[i];
        }
    }
    str[j] = '\0';
    
    lex->pos++; // Skip closing quote
    lex->col++;
    
    return str;
}

static void tokenize(Lexer* lex) {
    while (lex->source[lex->pos]) {
        skip_whitespace(lex);
        if (!lex->source[lex->pos]) break;
        
        char c = lex->source[lex->pos];
        
        // Single character tokens
        if (c == '(') { add_token(lex, TOK_LPAREN, NULL); lex->pos++; lex->col++; }
        else if (c == ')') { add_token(lex, TOK_RPAREN, NULL); lex->pos++; lex->col++; }
        else if (c == '{') { add_token(lex, TOK_LBRACE, NULL); lex->pos++; lex->col++; }
        else if (c == '}') { add_token(lex, TOK_RBRACE, NULL); lex->pos++; lex->col++; }
        else if (c == ';') { add_token(lex, TOK_SEMICOLON, NULL); lex->pos++; lex->col++; }
        else if (c == ',') { add_token(lex, TOK_COMMA, NULL); lex->pos++; lex->col++; }
        else if (c == '+') { add_token(lex, TOK_PLUS, NULL); lex->pos++; lex->col++; }
        else if (c == '-') { add_token(lex, TOK_MINUS, NULL); lex->pos++; lex->col++; }
        else if (c == '*') { add_token(lex, TOK_STAR, NULL); lex->pos++; lex->col++; }
        else if (c == '/') { add_token(lex, TOK_SLASH, NULL); lex->pos++; lex->col++; }
        
        // Multi-character operators
        else if (c == '=') {
            if (lex->source[lex->pos + 1] == '=') {
                add_token(lex, TOK_EQ, NULL);
                lex->pos += 2;
                lex->col += 2;
            } else {
                add_token(lex, TOK_ASSIGN, NULL);
                lex->pos++;
                lex->col++;
            }
        }
        else if (c == '<') {
            if (lex->source[lex->pos + 1] == '=') {
                add_token(lex, TOK_LEQ, NULL);
                lex->pos += 2;
                lex->col += 2;
            } else {
                add_token(lex, TOK_LT, NULL);
                lex->pos++;
                lex->col++;
            }
        }
        else if (c == '>') {
            if (lex->source[lex->pos + 1] == '=') {
                add_token(lex, TOK_GEQ, NULL);
                lex->pos += 2;
                lex->col += 2;
            } else {
                add_token(lex, TOK_GT, NULL);
                lex->pos++;
                lex->col++;
            }
        }
        else if (c == '!') {
            if (lex->source[lex->pos + 1] == '=') {
                add_token(lex, TOK_NEQ, NULL);
                lex->pos += 2;
                lex->col += 2;
            }
        }
        
        // String literals
        else if (c == '"') {
            char* str = extract_string(lex);
            add_token(lex, TOK_STRING, str);
        }
        
        // Numbers
        else if (is_digit(c)) {
            char* num = extract_number(lex);
            add_token(lex, TOK_NUMBER, num);
        }
        
        // Keywords and identifiers
        else if (is_alpha(c)) {
            char* id = extract_identifier(lex);
            
            // Check for keywords
            if (strcmp(id, "int") == 0) add_token(lex, TOK_INT, id);
            else if (strcmp(id, "char") == 0) add_token(lex, TOK_CHAR, id);
            else if (strcmp(id, "void") == 0) add_token(lex, TOK_VOID, id);
            else if (strcmp(id, "if") == 0) add_token(lex, TOK_IF, id);
            else if (strcmp(id, "else") == 0) add_token(lex, TOK_ELSE, id);
            else if (strcmp(id, "while") == 0) add_token(lex, TOK_WHILE, id);
            else if (strcmp(id, "for") == 0) add_token(lex, TOK_FOR, id);
            else if (strcmp(id, "return") == 0) add_token(lex, TOK_RETURN, id);
            else add_token(lex, TOK_IDENTIFIER, id);
        }
        else {
            kprintf("Unknown character: '%c' at line %d col %d\n", c, lex->line, lex->col);
            lex->pos++;
            lex->col++;
        }
    }
    
    add_token(lex, TOK_EOF, NULL);
}

// ===== CODE GENERATOR =====

static void codegen_init(CodeGen* gen) {
    gen->count = 0;
    gen->capacity = 256;
    gen->instructions = (Instruction*)kmalloc(sizeof(Instruction) * gen->capacity);
    
    gen->var_count = 0;
    gen->var_capacity = 32;
    gen->variables = (char**)kmalloc(sizeof(char*) * gen->var_capacity);
    
    gen->string_count = 0;
    gen->string_capacity = 32;
    gen->strings = (char**)kmalloc(sizeof(char*) * gen->string_capacity);
}

static void emit(CodeGen* gen, OpCode op, int operand) {
    if (gen->count >= gen->capacity) {
        gen->capacity *= 2;
        Instruction* new_inst = (Instruction*)kmalloc(sizeof(Instruction) * gen->capacity);
        for (int i = 0; i < gen->count; i++) {
            new_inst[i] = gen->instructions[i];
        }
        kfree(gen->instructions);
        gen->instructions = new_inst;
    }
    
    gen->instructions[gen->count].op = op;
    gen->instructions[gen->count].operand = operand;
    gen->instructions[gen->count].label = NULL;
    gen->count++;
}

static int add_string(CodeGen* gen, char* str) {
    if (gen->string_count >= gen->string_capacity) {
        gen->string_capacity *= 2;
        char** new_strs = (char**)kmalloc(sizeof(char*) * gen->string_capacity);
        for (int i = 0; i < gen->string_count; i++) {
            new_strs[i] = gen->strings[i];
        }
        kfree(gen->strings);
        gen->strings = new_strs;
    }
    
    gen->strings[gen->string_count] = str;
    return gen->string_count++;
}

// Simple code generation from tokens
static void generate_simple_code(CodeGen* gen, Token* tokens, int token_count) {
    int i = 0;
    
    // Find main function
    while (i < token_count && tokens[i].type != TOK_EOF) {
        if (tokens[i].type == TOK_IDENTIFIER && strcmp(tokens[i].value, "main") == 0) {
            // Skip to opening brace
            while (i < token_count && tokens[i].type != TOK_LBRACE) i++;
            i++; // Skip {
            
            // Generate code for main body
            while (i < token_count && tokens[i].type != TOK_RBRACE) {
                // Handle printf
                if (tokens[i].type == TOK_IDENTIFIER && strcmp(tokens[i].value, "printf") == 0) {
                    i++; // Skip printf
                    i++; // Skip (
                    
                    if (tokens[i].type == TOK_STRING) {
                        int str_idx = add_string(gen, tokens[i].value);
                        emit(gen, OP_PUSH, str_idx);
                        emit(gen, OP_SYSCALL, 1); // Syscall 1 = print string
                        i++;
                    }
                    
                    // Skip to semicolon
                    while (i < token_count && tokens[i].type != TOK_SEMICOLON) i++;
                }
                // Handle return
                else if (tokens[i].type == TOK_RETURN) {
                    i++;
                    if (tokens[i].type == TOK_NUMBER) {
                        int num = 0;
                        for (int j = 0; tokens[i].value[j]; j++) {
                            num = num * 10 + (tokens[i].value[j] - '0');
                        }
                        emit(gen, OP_PUSH, num);
                        emit(gen, OP_RET, 0);
                        i++;
                    }
                }
                
                i++;
            }
            
            emit(gen, OP_HALT, 0);
            return;
        }
        i++;
    }
}

// ===== VIRTUAL MACHINE =====

static void vm_init(VM* vm, Instruction* code, int code_size, char** strings, int string_count) {
    vm->stack = (int*)kmalloc(sizeof(int) * 256);
    vm->sp = 0;
    vm->locals = (int*)kmalloc(sizeof(int) * 64);
    vm->local_count = 0;
    vm->code = code;
    vm->ip = 0;
    vm->code_size = code_size;
    vm->strings = strings;
    vm->string_count = string_count;
    vm->running = 1;
}

static void vm_run(VM* vm) {
    while (vm->running && vm->ip < vm->code_size) {
        Instruction inst = vm->code[vm->ip];
        
        switch (inst.op) {
            case OP_PUSH:
                vm->stack[vm->sp++] = inst.operand;
                break;
                
            case OP_POP:
                vm->sp--;
                break;
                
            case OP_ADD: {
                int b = vm->stack[--vm->sp];
                int a = vm->stack[--vm->sp];
                vm->stack[vm->sp++] = a + b;
                break;
            }
            
            case OP_SUB: {
                int b = vm->stack[--vm->sp];
                int a = vm->stack[--vm->sp];
                vm->stack[vm->sp++] = a - b;
                break;
            }
            
            case OP_MUL: {
                int b = vm->stack[--vm->sp];
                int a = vm->stack[--vm->sp];
                vm->stack[vm->sp++] = a * b;
                break;
            }
            
            case OP_DIV: {
                int b = vm->stack[--vm->sp];
                int a = vm->stack[--vm->sp];
                if (b != 0) {
                    vm->stack[vm->sp++] = a / b;
                } else {
                    print_error("Division by zero");
                    vm->running = 0;
                }
                break;
            }
            
            case OP_SYSCALL: {
                if (inst.operand == 1) { // Print string
                    int str_idx = vm->stack[--vm->sp];
                    if (str_idx >= 0 && str_idx < vm->string_count) {
                        print_str(vm->strings[str_idx]);
                    }
                }
                break;
            }
            
            case OP_RET: {
                int ret_val = vm->stack[--vm->sp];
                kprintf("Program returned: %d\n", ret_val);
                vm->running = 0;
                break;
            }
            
            case OP_HALT:
                vm->running = 0;
                break;
                
            default:
                kprintf("Unknown opcode: %d\n", inst.op);
                vm->running = 0;
                break;
        }
        
        vm->ip++;
    }
}

// ===== MAIN COMPILER INTERFACE =====

int compile_and_run(char* source) {
    print_info("Compiling C code...");
    
    // Tokenize
    Lexer lex;
    lexer_init(&lex, source);
    tokenize(&lex);
    
    kprintf("Tokens: %d\n", lex.token_count);
    
    // Generate code
    CodeGen gen;
    codegen_init(&gen);
    generate_simple_code(&gen, lex.tokens, lex.token_count);
    
    kprintf("Instructions: %d\n", gen.count);
    print_success("Compilation complete");
    
    // Execute
    print_info("Executing program...");
    VM vm;
    vm_init(&vm, gen.instructions, gen.count, gen.strings, gen.string_count);
    vm_run(&vm);
    
    print_success("Execution complete");
    
    // Cleanup
    kfree(lex.tokens);
    kfree(gen.instructions);
    kfree(gen.variables);
    kfree(vm.stack);
    kfree(vm.locals);
    
    return 0;
}

int compile_file(const char* filename) {
    print_info("Compiling C file");
    kprintf("Loading: %s\n", filename);
    
    if (!fat32_file_exists(filename))
    {
        kprintf("File not found: %s\n", filename);
        return -1;
    }

    uint32_t size = fat32_get_file_size(filename);
    if (size == 0 || size == 0xFFFFFFFF) {
        print_error("File not found or invalid size");
        return -1;
    }

    kprintf("Size: %u bytes\n", size);
    
    uint8_t *source = kmalloc(size + 1);
    if (!source) {
        print_error("Failed to allocate memory");
        return -1;
    }
    
    int bytes = fat32_read_file(filename, source, size);
    if (bytes < 0)
    {
        print_error("Failed to read file");
        kfree(source);
        return -1;
    }
    source[size] = '\0';
    
    print_success("File loaded");
    kprintf("Size: %u bytes\n", size);
    
    int result = compile_and_run(source);
    
    kfree(source);
    return result;
}

void cmd_compile(const char* args) {
    if (!args || args[0] == '\0') {
        print_error("Usage: compile <filename.c>");
        return;
    }
    
    while (*args == ' ') args++;
    
    char filename[128];
    int i = 0;
    while (args[i] && args[i] != ' ' && args[i] != '\n' && i < 127) {
        filename[i] = args[i];
        i++;
    }
    filename[i] = '\0';
    
    if (!kstr_contains(filename, ".c")) {
        if (i < 125) {
            filename[i] = '.';
            filename[i+1] = 'c';
            filename[i+2] = '\0';
        }
    }
    
    compile_file(filename);
}
