#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "passenger.h"
#include "ipc.h"
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

void run_passenger(int id) {
    key_t key = ftok("/tmp", 'P');
    int msgid = msgget(key, 0666);
    if (msgid == -1)  _exit(1);

    srand(getpid());
    int baggage = rand() % 40 + 1; //1-40kg
    int gender  = rand() % 2;      // male/female
    bool vip    = (rand() % 5 == 0); // ok 20% vip
    int waited  = 0;               // frustracja

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d %s gender=%s\n",
            id,
            vip ? "VIP" : "REGULAR",
            gender == MALE ? "MALE" : "FEMALE");

    PassengerMessage msg;
    msg.mtype = MSG_TYPE_PASSENGER;
    msg.passenger_id = id;
    msg.pid = getpid();
    msg.baggage_weight = baggage;

    msgsnd(msgid, &msg, sizeof(PassengerMessage) - sizeof(long), 0);

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d baggage=%d kg sent for check\n",
            id, baggage);

    DecisionMessage decision;
    msgrcv(msgid,
            &decision,
            sizeof(DecisionMessage) - sizeof(long),
            MSG_TYPE_DECISION_BASE + id,
            0);

    if (!decision.accepted) {
        dprintf(STDOUT_FILENO, "[PASSENGER] id=%d REJECTED (baggage too heavy)\n", id);
        return;
    }

    dprintf(STDOUT_FILENO, "[PASSENGER] id=%d ACCEPTED\n", id);

/*=== kontrola bezp ===*/
    key_t shm_key = ftok("/tmp", 'M');
    int shmid = shmget(shm_key, sizeof(SecurityStation) * NUM_STATIONS, 0666);
    SecurityStation* stations = (SecurityStation*) shmat(shmid, nullptr, 0);

    if (stations == (void*) -1) {
    perror("shmat stations");
    _exit(1);
    }

    key_t mutex_key = ftok("/tmp", 'X');
    int mutex = semget(mutex_key, 1, 0666);

    int station = rand() % NUM_STATIONS;

    //proba wejscia
    while (true) {
        sem_down(mutex, 0);

        if (stations[station].count == 0) {
            stations[station].gender = gender;
        }

        if (stations[station].gender == gender &&
            stations[station].count < 2) {
            stations[station].count++;
            sem_up(mutex, 0);
            break;
        }

        sem_up(mutex, 0);

        if (!vip) {
            waited++;
            if (waited > 3) {
                dprintf(STDOUT_FILENO, "[PASSENGER] id=%d FRUSTRATED, leaving security queue\n", id);
                return;
            }
        }
        sleep(1);
    }

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d ENTER security station %d\n",
            id, station);

    sleep(2); // symulacja kontroli

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d LEAVE security station %d\n",
            id, station);

    //wyjscie
    sem_down(mutex, 0);
    stations[station].count--;
    if (stations[station].count == 0) {
        stations[station].gender = -1;
    }
    sem_up(mutex, 0);

    key_t ferry_key = ftok("/tmp", 'F');
    if (ferry_key == -1) {
        perror("[PASSENGER] ftok ferry_key");
        _exit(1);
    }
    
    int ferry_shmid = -1;
    // retry przez max 5s
    for (int attempt = 0; attempt < 50; attempt++) {
        ferry_shmid = shmget(ferry_key, sizeof(FerryState), 0666);
        if (ferry_shmid != -1) {
            break;
        }
        usleep(100000);
    }
    
    if (ferry_shmid == -1) {
        dprintf(STDERR_FILENO, "[PASSENGER %d] shmget ferry failed after retries\n", id);
        perror("[PASSENGER] shmget ferry");
        _exit(1);
    }
    
    FerryState* ferry = (FerryState*) shmat(ferry_shmid, nullptr, 0);

    if (ferry == (void*) -1) {
        dprintf(STDERR_FILENO, "[PASSENGER %d] shmat ferry failed, shmid=%d\n", id, ferry_shmid);
        perror("[PASSENGER] shmat ferry");
        _exit(1);
    }

    sem_down(mutex, 0);
    ferry->in_waiting++;
    int current_waiting = ferry->in_waiting;
    sem_up(mutex, 0);
    
    dprintf(STDOUT_FILENO, 
            "[PASSENGER] id=%d ENTERED waiting area (%d waiting)\n",
            id, current_waiting);

/*=== poczekalnia -> trap -> prom + frustracja ===*/
    // trap
    key_t trap_key = ftok("/tmp", 'T');
    int trap_sem = semget(trap_key, 1, 0666);

    struct sembuf sb = {0, -1, IPC_NOWAIT};

    while (semop(trap_sem, &sb, 1) == -1) {
        if (!vip) {
            waited++;
            if (waited > 3) {
                dprintf(STDOUT_FILENO,
                        "[PASSENGER] id=%d FRUSTRATED, leaving gangway queue\n",
                        id);
                sem_down(mutex, 0);
                ferry->in_waiting--;
                sem_up(mutex, 0);
                
                return;
            }
        }
        sleep(1);
    }

    sem_down(mutex, 0);
    ferry->in_waiting--;     // wychodzi z poczekalni
    
    if (ferry->onboard < FERRY_CAPACITY) {
        ferry->onboard++;    // wchodzi na prom
        dprintf(STDOUT_FILENO, "[PASSENGER] id=%d BOARDED ferry (%d waiting, %d/%d onboard)\n", 
                id, ferry->in_waiting, ferry->onboard, FERRY_CAPACITY);
    }
    sem_up(mutex, 0);

    sem_up(trap_sem, 0);
}