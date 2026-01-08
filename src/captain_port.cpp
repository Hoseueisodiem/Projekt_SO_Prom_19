#include <cstdio>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include "captain_port.h"
#include "ipc.h"

#define MAX_BAGGAGE 20 // max dop waga kg
#define NUM_STATIONS 3
#define MAX_PER_STATION 2

void run_captain_port() {
    key_t key = ftok(".", 'P');
    int msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN PORT] failed to create message queue\n");
        _exit(1);
    }


    //semafory kont bezp
    key_t sem_key = ftok(".", 'S');
    int semid = semget(sem_key, NUM_STATIONS, IPC_CREAT | 0666);
    if (semid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN PORT] failed to create semaphores\n");
        _exit(1);
    }

    //max 2 na stanowisko
    for (int i = 0; i < NUM_STATIONS; i++){
        semctl(semid, i, SETVAL, MAX_PER_STATION);
    }

    dprintf(STDOUT_FILENO, "[CAPTAIN PORT] PID=%d waiting for passengers...\n", getpid());

    PassengerMessage msg;

    while (true) {
        if (msgrcv(msgid, &msg, sizeof(PassengerMessage) - sizeof(long), MSG_TYPE_PASSENGER, 0) == -1) {
            dprintf(STDERR_FILENO, "[CAPTAIN PORT] msgrcv error\n");
            continue;
        }

        dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Passenger id=%d baggage=%d kg\n", msg.passenger_id, msg.baggage_weight);

        DecisionMessage decision;
        decision.mtype = MSG_TYPE_DECISION;
        decision.passenger_id = msg.passenger_id;

        if (msg.baggage_weight > MAX_BAGGAGE) {
            decision.accepted = 0;
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Passenger id=%d REJECTED (baggage too heavy)\n", msg.passenger_id);
        } else {
            decision.accepted = 1;
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Passenger id=%d ACCEPTED\n", msg.passenger_id);
        }

        msgsnd(msgid, &decision, sizeof(DecisionMessage) - sizeof(long), 0);
    }
}