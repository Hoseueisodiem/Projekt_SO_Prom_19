#include <unistd.h>
#include <sys/wait.h>
#include <iostream>
#include <cstdio>
#include <signal.h>

#include "passenger.h"
#include "captain_port.h"
#include "captain_ferry.h"

pid_t ferry_pid = -1;

int main() {
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);

    pid_t pid;

    pid = fork();
    if (pid == 0) {
        run_captain_port();
        _exit(0);
    }
    sleep(1);

    pid = fork();
    if (pid == 0) {
        run_captain_ferry();
        _exit(0);
    }
    ferry_pid = pid;

    sleep(1);

    for (int i = 0; i < 3; i++) {
        pid = fork();
        if (pid == 0) {
            run_passenger(i);
            _exit(0);
        }
        usleep(200000); //odstep miedzy pasazerami
    }

    while (wait(nullptr) > 0);
    return 0;
}

