#include <unistd.h>
#include <sys/wait.h>
#include <iostream>

#include "passenger.h"
#include "captain_port.h"
#include "captain_ferry.h"

int main() {
    pid_t pid;

    pid = fork();
    if (pid == 0) {
        run_captain_port();
        return 0;
    }

    pid = fork();
    if (pid == 0) {
        run_captain_ferry();
        return 0;
    }

    for (int i = 0; i < 3; i++) {
        pid = fork();
        if (pid == 0) {
            run_passenger(i);
            return 0;
        }
    }

    while (wait(nullptr) > 0);
    return 0;
}
