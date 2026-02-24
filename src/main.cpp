#include <unistd.h>
#include <sys/wait.h>
#include <iostream>
#include <cstdio>
#include <ctime>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>

#include "passenger.h"
#include "captain_port.h"
#include "captain_ferry.h"
#include "security_station.h"
#include "security.h"

int main() {
    srand(time(nullptr));

    int log_fd = open("simulation.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd == -1) {
        perror("Failed to open simulation.log");
        return 1;
    }

    if (dup2(log_fd, STDOUT_FILENO) == -1) {
        perror("Failed to redirect stdout");
        close(log_fd);
        return 1;
    }
    if (dup2(log_fd, STDERR_FILENO) == -1) {
        perror("Failed to redirect stderr");
        close(log_fd);
        return 1;
    }

    close(log_fd);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    pid_t pid;
    
    // Tworzenie procesu portu
    pid_t port_pid = fork();
    if (port_pid == 0) {
        // Proces dziecko - port
        run_captain_port();
        _exit(0);
    }
    
    dprintf(STDOUT_FILENO, "[MAIN] Started port with PID=%d\n", port_pid);
    sleep(1);  // czas portowi na inicjalizację
    
    // Tworzenie procesow promow
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

    // Tworzenie procesow stanowisk kontroli bezpieczenstwa (przed pasazerami)
    dprintf(STDOUT_FILENO, "[MAIN] Starting %d security stations...\n", NUM_STATIONS);
    for (int i = 0; i < NUM_STATIONS; i++) {
        pid = fork();
        if (pid == 0) {
            run_security_station(i);
            _exit(0);
        }
        dprintf(STDOUT_FILENO, "[MAIN] Started security station %d with PID=%d\n", i, pid);
    }

    sleep(1);  // czas na inicjalizacje stanowisk

    // Tworzenie procesow pasazerow
    int num_passengers = 1000;
    for (int i = 0; i < num_passengers; i++) {
        pid = fork();
        if (pid == 0) {
            run_passenger(i);
            _exit(0);
        }
        usleep(10000); //odstep miedzy pasazerami
    }

    // Liczba wszystkich procesów potomnych
    int total_processes = 1 + NUM_FERRIES + NUM_STATIONS + num_passengers;  // port + promy + stanowiska + pasazerowie
    
    dprintf(STDOUT_FILENO,
            "[MAIN] Created %d processes total (1 port + %d ferries + %d stations + %d passengers)\n",
            total_processes, NUM_FERRIES, NUM_STATIONS, num_passengers);
    dprintf(STDOUT_FILENO, "[MAIN] Waiting 200 seconds before closing port...\n");
    sleep(200);
    
    dprintf(STDOUT_FILENO, "[MAIN] Sending SIGUSR2 to close port (PID=%d)\n", port_pid);
    if (kill(port_pid, SIGUSR2) == -1) {
        perror("[MAIN] kill SIGUSR2 failed");
    } else {
        dprintf(STDOUT_FILENO, "[MAIN] Port closure signal sent successfully\n");
    }
    
    // Czekanie na wszystkie procesy potomne
    dprintf(STDERR_FILENO, "[MAIN] Waiting for %d processes to finish...\n", total_processes);
    for (int i = 0; i < total_processes; i++) {
        int status;
        pid_t finished_pid = wait(&status);
        if (finished_pid > 0 && i % 100 == 0) {  // Log co 100 procesow
            dprintf(STDERR_FILENO, "[MAIN] Progress: %d/%d processes finished\n", i+1, total_processes);
        }
    }
    
    dprintf(STDERR_FILENO, "[MAIN] All %d processes finished. Exiting.\n", total_processes);
    return 0;
}