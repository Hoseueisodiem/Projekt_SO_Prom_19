#include <cstdio>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include "captain_port.h"
#include "ipc.h"
#include "security.h"

#define MAX_BAGGAGE 20 // max dop waga kg

void run_captain_port() {
    key_t key = ftok(".", 'P');
    int msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN PORT] failed to create message queue\n");
        _exit(1);
    }

    // pamiec wspoldzielona stan stanowisk
    key_t shm_key = ftok(".", 'M');
    int shmid = shmget(shm_key,
                      sizeof(SecurityStation) * NUM_STATIONS, IPC_CREAT | 0666);
    if (shmid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN PORT] failed to create shared memory\n");
        _exit(1);
    }

    SecurityStation* stations = (SecurityStation*) shmat(shmid, nullptr, 0);

    for (int i = 0; i < NUM_STATIONS; i++) {
        stations[i].count = 0;
        stations[i].gender = -1; // brak
    }

    // mutex do ochrony stanu
    key_t mutex_key = ftok(".", 'X');
    int mutex = semget(mutex_key, 1, IPC_CREAT | 0666);
    semctl(mutex, 0, SETVAL, 1);

    // trap K
    key_t trap_key = ftok(".", 'T');
    int trap_sem = semget(trap_key, 1, IPC_CREAT | 0666);
    semctl(trap_sem, 0, SETVAL, GANGWAY_CAPACITY);

    // prom P
    key_t ferry_key = ftok(".", 'F');
    int ferry_shmid = shmget(ferry_key, sizeof(FerryState), IPC_CREAT | 0666);
    FerryState* ferry = (FerryState*) shmat(ferry_shmid, nullptr, 0);
    ferry->onboard = 0;

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