#include "platform.h"

#include <stdio.h>
#include <string.h>

#include "cli.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "start") == 0)   return cmd_start(argc, argv);
    if (strcmp(argv[1], "stop") == 0)    return cmd_stop();
    if (strcmp(argv[1], "status") == 0)  return cmd_status();
    if (strcmp(argv[1], "version") == 0) return cmd_version();

    fprintf(stderr, "unknown command: %s\n\n", argv[1]);
    print_usage();
    return 1;
}