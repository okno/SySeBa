#include "syseba_internal.h"

int main(int argc, char **argv)
{
    syseba_options_t options;
    char error[1024] = {0};
    int result;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    syseba_options_defaults(&options);
    result = syseba_cli_parse(argc,
                              argv,
                              &options,
                              error,
                              sizeof(error));
    if (result > 0) {
        return 0;
    }
    if (result < 0) {
        fprintf(stderr,
                "%s: %s\n\n",
                options.language == SYSEBA_LANGUAGE_IT ? "Errore" : "Error",
                error);
        syseba_cli_usage(stderr, options.language);
        return 2;
    }
    return syseba_cli_execute(&options);
}
