#include <unistd.h>
#include <sys/wait.h>
#include <iostream>
#include <cstdio>
#include <signal.h>

#include "passenger.h"
#include "captain_port.h"
#include "captain_ferry.h"

int main() {
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);

    pid_t pid;

    pid = fork();
    if (pid == 0) {
        run_captain_port();
        _exit(0);
    }
    pid_t port_pid = pid;
    sleep(1);

    pid = fork();
    if (pid == 0) {
        run_captain_ferry();
        _exit(0);
    }
    sleep(1);

    for (int i = 0; i < 10; i++) {
        pid = fork();
        if (pid == 0) {
            run_passenger(i);
            _exit(0);
        }
        usleep(200000); //odstep miedzy pasazerami
    }

    dprintf(STDOUT_FILENO, "[MAIN] Waiting 15 seconds before closing port...\n");
    sleep(15);
    
    dprintf(STDOUT_FILENO, "[MAIN] Sending SIGUSR2 to close port (PID=%d)\n", port_pid);
    if (kill(port_pid, SIGUSR2) == -1) {
        perror("[MAIN] kill SIGUSR2 failed");
    } else {
        dprintf(STDOUT_FILENO, "[MAIN] Port closure signal sent successfully\n");
    }

    // czekanie na procesy wszystkie
    while (wait(nullptr) > 0);
    
    dprintf(STDOUT_FILENO, "[MAIN] All processes finished. Exiting.\n");
    return 0;
}

