#include <cstdio>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <signal.h>
#include <ctime>
#include "captain_ferry.h"
#include "security.h"
#include "ipc.h"

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

// sygnal 1
volatile sig_atomic_t early_departure = 0;

static void handle_sigusr1(int) {
    early_departure = 1;
}

// kapitan czeka t1 sekund, sprawdza czy trap psuty, odplywa, reset promu

void run_captain_ferry() {
    signal(SIGUSR1, handle_sigusr1);
    // mutex
    key_t mutex_key = ftok("/tmp", 'X');
    int mutex = semget(mutex_key, 1, 0666);

    // trap
    key_t trap_key = ftok("/tmp", 'T');
    int trap_sem = semget(trap_key, 1, 0666);

    // prom
    key_t ferry_key = ftok("/tmp", 'F');
    if (ferry_key == -1) {
        perror("[CAPTAIN FERRY] ftok ferry_key");
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
        dprintf(STDERR_FILENO, "[CAPTAIN FERRY] shmget failed after retries, key=%d\n", ferry_key);
        perror("[CAPTAIN FERRY] shmget ferry");
        _exit(1);
    }
    
    dprintf(STDOUT_FILENO, "[CAPTAIN FERRY] shmget success, shmid=%d\n", ferry_shmid);
    
    FerryState* ferry = (FerryState*) shmat(ferry_shmid, nullptr, 0);

    if (ferry == (void*) -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN FERRY] shmat failed, shmid=%d\n", ferry_shmid);
        perror("[CAPTAIN FERRY] shmat ferry");
        _exit(1);
    }

    // kolejka komunikatow
    key_t msg_key = ftok("/tmp", 'P');
    int msgid = msgget(msg_key, 0666);
    if (msgid == -1) {
        perror("[CAPTAIN FERRY] msgget");
        _exit(1);
    }

    dprintf(STDOUT_FILENO, "[CAPTAIN FERRY] PID=%d\n", getpid());

    time_t last_departure = time(nullptr);
    
    while (true) {
        sleep(1);

        time_t now = time(nullptr);
        bool time_to_depart = (now - last_departure >= DEPARTURE_TIME);

        // sprawdz trap
        int trap_val = semctl(trap_sem, 0, GETVAL);
        if (trap_val != GANGWAY_CAPACITY) {
            continue;  // czy ktos na trapie
        }

        if (time_to_depart || early_departure) {
            // sprawdz czy sa pasazerowie
            sem_down(mutex, 0);
            int onboard = ferry->onboard;
            int in_waiting = ferry->in_waiting;
            sem_up(mutex, 0);
            
            if (onboard == 0 && early_departure) {
                dprintf(STDOUT_FILENO, 
                    "[CAPTAIN FERRY] Early departure signal received, but no passengers onboard yet (%d still in waiting). Waiting...\n",
                    in_waiting);
                
                // poczekanie max 5s na pasazerow
                bool passengers_arrived = false;
                for (int wait_time = 0; wait_time < 10; wait_time++) {  // 5s
                    usleep(500000);  // 500ms
                    
                    sem_down(mutex, 0);
                    onboard = ferry->onboard;
                    sem_up(mutex, 0);
                    
                    if (onboard > 0) {
                        dprintf(STDOUT_FILENO, 
                            "[CAPTAIN FERRY] Passengers arrived (%d onboard), ready to depart\n", onboard);
                        passengers_arrived = true;
                        break;
                    }
                }
                
                if (!passengers_arrived) {
                    dprintf(STDOUT_FILENO, 
                        "[CAPTAIN FERRY] Still no passengers after waiting. Canceling early departure.\n");
                    early_departure = 0;
                    continue;
                }
            }
            
            // odplyniecie
            sem_down(mutex, 0);
            onboard = ferry->onboard;
            ferry->onboard = 0;
            sem_up(mutex, 0);

            dprintf(STDOUT_FILENO,
                "[CAPTAIN FERRY] DEPARTURE%s, onboard=%d\n",
                early_departure ? " (EARLY)" : "",
                onboard);

            early_departure = 0;
            last_departure = now;
            
            // wyslanie wiad do kap portu
            FerryDepartureMessage departure_msg;
            departure_msg.mtype = MSG_TYPE_FERRY_DEPARTED;
            departure_msg.ferry_id = 0;
            departure_msg.passengers_count = onboard;
            
            if (msgsnd(msgid, &departure_msg, 
                       sizeof(FerryDepartureMessage) - sizeof(long), 0) == -1) {
                perror("[CAPTAIN FERRY] msgsnd departure message failed");
            } else {
                dprintf(STDOUT_FILENO, 
                    "[CAPTAIN FERRY] Sent departure notification to port\n");
            }
        }
    }
}
