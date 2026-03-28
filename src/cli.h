#ifndef CLI_H
#define CLI_H

int cli_run(int argc, char** argv);
int cli_debug(int argc, char** argv);
int cli_compile(int argc, char** argv);
int cli_test(int argc, char** argv);
int cli_mod(int argc, char** argv);
int cli_lsp(int argc, char** argv);
int cli_dap(int argc, char** argv);

#endif
