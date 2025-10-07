#ifndef COMPILER_H
#define COMPILER_H

int compile_and_run(char* source);

int compile_file(const char* filename);

void cmd_compile(const char* args);

#endif