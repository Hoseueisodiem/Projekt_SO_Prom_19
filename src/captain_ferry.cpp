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
    int mutex = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        mutex = semget(mutex_key, 1, 0666);
        if (mutex != -1) break;
        usleep(100000);
    }
    if (mutex == -1) {
        perror("[CAPTAIN FERRY] semget mutex failed after retries");
        _exit(1);
    }

    // trap
    key_t trap_key = ftok("/tmp", 'T');
    int trap_sem = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        trap_sem = semget(trap_key, 1, 0666);
        if (trap_sem != -1) break;
        usleep(100000);
    }
    if (trap_sem == -1) {
        perror("[CAPTAIN FERRY] semget trap failed after retries");
        _exit(1);
    }

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

    key_t port_state_key = ftok("/tmp", 'S');
    int port_shmid = -1;
    
    for (int attempt = 0; attempt < 50; attempt++) {
        port_shmid = shmget(port_state_key, sizeof(PortState), 0666);
        if (port_shmid != -1) break;
        usleep(100000);
    }
    
    if (port_shmid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN FERRY] shmget port_state failed\n");
        perror("[CAPTAIN FERRY] shmget port_state");
        _exit(1);
    }
    
    PortState* port_state = (PortState*) shmat(port_shmid, nullptr, 0);
    if (port_state == (void*) -1) {
        perror("[CAPTAIN FERRY] shmat port_state");
        _exit(1);
    }

    // zapisz swoj PID do pamieci wspoldzielonej
    sem_down(mutex, 0);
    port_state->ferry_captain_pid = getpid();
    sem_up(mutex, 0);

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

        if (time_to_depart || early_departure) {
            bool trap_clear = false;

            for (int i = 0; i < 30; i++) {  // 3s
                int trap_val = semctl(trap_sem, 0, GETVAL);
                if (trap_val == GANGWAY_CAPACITY) {
                    trap_clear = true;
                    break;
                }
                usleep(100000);
            }
            
            if (!trap_clear) {
                dprintf(STDOUT_FILENO, 
                    "[CAPTAIN FERRY] Trap still occupied after waiting, skipping departure cycle\n");
                if (early_departure) {
                    early_departure = 0;
                }
                continue;
            }
            
            // trap pusty, kontynuacja
            
            // sprawdz czy sa pasazerowie
            sem_down(mutex, 0);
            int accepting = port_state->accepting_passengers;
            int onboard = ferry->onboard;
            int in_waiting = ferry->in_waiting;
            int passengers_left = port_state->passengers_onboard;
            sem_up(mutex, 0);

            // jesli port zamkniety i brak pasazerow koncz
            if (!accepting && passengers_left == 0 && onboard == 0) {
                dprintf(STDOUT_FILENO, 
                    "[CAPTAIN FERRY] Port closed, no passengers. Shutting down.\n");
                break;
            }
            
            // jesli brak pasazerow na promie
            if (onboard == 0) {
                if (early_departure) {
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
            } else {
                dprintf(STDOUT_FILENO, "[CAPTAIN FERRY] Departure time reached, but no passengers. Skipping departure.\n");
                last_departure = now;
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

            // symulacja podrozu
            if (onboard > 0) {  // tylko jesli sa pasazerowie
                dprintf(STDOUT_FILENO, 
                    "[CAPTAIN FERRY] Traveling with %d passengers (estimated %d seconds)...\n",
                    onboard, TRAVEL_TIME);
                
                sleep(TRAVEL_TIME);  // symulacja
                
                dprintf(STDOUT_FILENO, 
                    "[CAPTAIN FERRY] Arrived at destination, passengers disembarking...\n");
                
                // zmniejsz passengers_onboard
                sem_down(mutex, 0);
                port_state->passengers_onboard -= onboard;
                int remaining = port_state->passengers_onboard;
                sem_up(mutex, 0);
                
                dprintf(STDOUT_FILENO, 
                    "[CAPTAIN FERRY] Returned to port. %d passengers still onboard (all ferries).\n",
                    remaining);
            }
            
            // sprawdzanie warunku zakonczenia
            sem_down(mutex, 0);
            accepting = port_state->accepting_passengers;
            passengers_left = port_state->passengers_onboard;
            sem_up(mutex, 0);
            
            if (!accepting && passengers_left == 0) {
                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY] Port closed and no passengers remaining. Shutting down.\n");
                break;
            }
        }
    }

    shmdt(port_state);
    shmdt(ferry);
}
