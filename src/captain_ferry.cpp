#include <cstdio>
#include <unistd.h>
#include "captain_ferry.h"

void run_captain_ferry() {
    dprintf(STDOUT_FILENO,
            "[CAPTAIN FERRY] PID=%d\n",
            getpid());
}
