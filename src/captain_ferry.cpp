#include <cstdio>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include "captain_ferry.h"
#include "security.h"

//semafory
static void sem_down(int semid, int semnum) {
    struct sembuf sb = {
        static_cast<unsigned short>(semnum), -1, 0};
    semop(semid, &sb, 1);
}

static void sem_up(int semid, int semnum) {
    struct sembuf sb = {
        static_cast<unsigned short>(semnum), 1, 0};
    semop(semid, &sb, 1);
}

// kapitan czeka t1 sekund, sprawdza czy trap psuty, odplywa, reset promu

void run_captain_ferry() {
    // mutex
    key_t mutex_key = ftok(".", 'X');
    int mutex = semget(mutex_key, 1, 0666);

    // trap
    key_t trap_key = ftok(".", 'T');
    int trap_sem = semget(trap_key, 1, 0666);

    // prom
    key_t ferry_key = ftok(".", 'F');
    int ferry_shmid = shmget(ferry_key, sizeof(FerryState), 0666);
    FerryState* ferry = (FerryState*) shmat(ferry_shmid, nullptr, 0);

    dprintf(STDOUT_FILENO,
            "[CAPTAIN FERRY] PID=%d\n",
            getpid());
    
     while (true) {
        sleep(DEPARTURE_TIME);

        int trap_val = semctl(trap_sem, 0, GETVAL);

        if (trap_val != GANGWAY_CAPACITY) {
            dprintf(STDOUT_FILENO, "[CAPTAIN FERRY] waiting for empty gangway...\n"); continue;
        }

        sem_down(mutex, 0);
        int onboard = ferry->onboard;
        ferry->onboard = 0;
        sem_up(mutex, 0);

        dprintf(STDOUT_FILENO, "[CAPTAIN FERRY] DEPARTURE, onboard=%d\n", onboard);
    }
}
