#include <unistd.h>
#include <sys/wait.h>
#include <iostream>
#include <cstdio>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#include "passenger.h"
#include "captain_port.h"
#include "captain_ferry.h"
#include "security.h"

void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved_errno;
}

int main() {
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction SIGCHLD");
        return 1;
    }

    // stdout do pliku
    int log_fd = open("simulation.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd != -1) {
        dup2(log_fd, STDOUT_FILENO);
        close(log_fd);
        // wylacz buforowanie dla natychmiastowego zapisu
        setbuf(stdout, NULL);
    } else {
        perror("Failed to open simulation.log");
    }
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

    dprintf(STDOUT_FILENO, "[MAIN] Starting %d ferries...\n", NUM_FERRIES);

        for (int i = 0; i < NUM_FERRIES; i++) {
        pid = fork();
        if (pid == 0) {
            run_captain_ferry(i);  // przekazanie id promu
            _exit(0);
        }
        dprintf(STDOUT_FILENO, "[MAIN] Started ferry %d with PID=%d\n", i, pid);
        usleep(100000);  // 100ms delay
    }
    
    sleep(1);

    for (int i = 0; i < 30; i++) {
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

