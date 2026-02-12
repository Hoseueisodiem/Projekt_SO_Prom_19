#include <cstdio>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <signal.h>
#include <ctime>
#include <sys/prctl.h>
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

void run_captain_ferry(int ferry_id) {
    prctl(PR_SET_NAME, "kapitan_prom");
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
        trap_sem = semget(trap_key, NUM_FERRIES, 0666);
        if (trap_sem != -1) break;
        usleep(100000);
    }
    if (trap_sem == -1) {
        perror("[CAPTAIN FERRY] semget trap failed after retries");
        _exit(1);
    }

    // port state
    key_t port_state_key = ftok("/tmp", 'S');
    int port_shmid = -1;
    
    for (int attempt = 0; attempt < 50; attempt++) {
        port_shmid = shmget(port_state_key, sizeof(PortState), 0666);
        if (port_shmid != -1) break;
        usleep(100000);
    }
    
    if (port_shmid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN FERRY %d] shmget port_state failed\n", ferry_id);
        perror("[CAPTAIN FERRY] shmget port_state");
        _exit(1);
    }
    
    PortState* port_state = (PortState*) shmat(port_shmid, nullptr, 0);
    if (port_state == (void*) -1) {
        perror("[CAPTAIN FERRY] shmat port_state");
        _exit(1);
    }

    // wskaznik na prom
    Ferry* my_ferry = &port_state->ferries[ferry_id];

    // kolejka komunikatow
    key_t msg_key = ftok("/tmp", 'P');
    int msgid = msgget(msg_key, 0666);
    if (msgid == -1) {
        perror("[CAPTAIN FERRY] msgget");
        _exit(1);
    }

    // zapisz PID kapitana w strukturze promu
    sem_down(mutex, 0);
    my_ferry->captain_pid = getpid();
    sem_up(mutex, 0);

    dprintf(STDOUT_FILENO, "[CAPTAIN FERRY %d] PID=%d, capacity=%d\n",
            ferry_id, getpid(), my_ferry->capacity);

    time_t last_departure = time(nullptr);
    
    while (true) {
        sleep(1);

        time_t now = time(nullptr);
        bool time_to_depart = (now - last_departure >= DEPARTURE_TIME);

        if (time_to_depart || early_departure) {
            // sprawdz czy prom jest pelny
            sem_down(mutex, 0);
            bool ferry_full = (my_ferry->onboard >= my_ferry->capacity);
            sem_up(mutex, 0);

            // jesli prom nie pelny czekaj az trap pusty
            if (!ferry_full) {
                bool trap_clear = false;

                for (int i = 0; i < 30; i++) {  // 3s
                    int trap_val = semctl(trap_sem, ferry_id, GETVAL);
                    if (trap_val == GANGWAY_CAPACITY) {
                        trap_clear = true;
                        break;
                    }
                    usleep(100000);
                }

                if (!trap_clear) {
                    dprintf(STDOUT_FILENO,
                        "[CAPTAIN FERRY %d] Trap occupied, skipping departure\n", ferry_id);
                    if (early_departure) {
                        early_departure = 0;
                    }
                    continue;
                }
            } else {
                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY %d] Ferry full (%d/%d), departing regardless of trap state\n",
                    ferry_id, my_ferry->onboard, my_ferry->capacity);
            }

            // trap pusty lub prom pelny, kontynuacja
            
            // sprawdz czy sa pasazerowie
            sem_down(mutex, 0);
            int accepting = port_state->accepting_passengers;
            int onboard = my_ferry->onboard;
            int in_waiting = my_ferry->in_waiting;
            int passengers_left = port_state->passengers_onboard;
            sem_up(mutex, 0);

            // jesli port zamkniety i brak pasazerow (na promach + w poczekalni), koncz
            int waiting_for_me = my_ferry->in_waiting + my_ferry->in_waiting_vip;
            if (!accepting && passengers_left == 0 && onboard == 0 && waiting_for_me == 0) {
                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY %d] Port closed, no passengers anywhere. Shutting down.\n", ferry_id);

                sem_down(mutex, 0);
                my_ferry->status = FERRY_SHUTDOWN;
                sem_up(mutex, 0);
                break;
            }
            
            // jesli brak pasazerow na promie
            if (onboard == 0) {
                if (early_departure) {
                    dprintf(STDOUT_FILENO,
                        "[CAPTAIN FERRY %d] Early departure signal received, but no passengers onboard yet (%d still in waiting). Waiting...\n",
                        ferry_id, in_waiting);

                    // poczekanie max 5s na pasazerow
                    bool passengers_arrived = false;
                    for (int wait_time = 0; wait_time < 10; wait_time++) {  // 5s
                        usleep(500000);  // 500ms

                        sem_down(mutex, 0);
                        onboard = my_ferry->onboard;
                        sem_up(mutex, 0);

                        if (onboard > 0) {
                            dprintf(STDOUT_FILENO,
                                "[CAPTAIN FERRY %d] Passengers arrived (%d onboard), ready to depart\n",
                                ferry_id, onboard);
                            passengers_arrived = true;
                            break;
                        }
                    }

                    if (!passengers_arrived) {
                        dprintf(STDOUT_FILENO,
                            "[CAPTAIN FERRY %d] Still no passengers after waiting. Canceling early departure.\n",
                            ferry_id);
                        early_departure = 0;
                        continue;
                    }
                } else {
                    dprintf(STDOUT_FILENO,
                        "[CAPTAIN FERRY %d] Departure time reached, but no passengers. Skipping departure.\n",
                        ferry_id);
                    last_departure = now;
                    continue;
                }
            }

            // ustaw status TRAVELING
            sem_down(mutex, 0);
            onboard = my_ferry->onboard;
            my_ferry->onboard = 0;
            my_ferry->status = FERRY_TRAVELING;
            sem_up(mutex, 0);

            dprintf(STDOUT_FILENO,
                "[CAPTAIN FERRY %d] DEPARTURE%s, onboard=%d\n",
                ferry_id,
                early_departure ? " (EARLY)" : "",
                onboard);

            early_departure = 0;
            last_departure = now;

            FerryDepartureMessage departure_msg;
            departure_msg.mtype = MSG_TYPE_FERRY_DEPARTED;
            departure_msg.ferry_id = ferry_id;
            departure_msg.passengers_count = onboard;

            if (msgsnd(msgid, &departure_msg,
                       sizeof(FerryDepartureMessage) - sizeof(long), 0) == -1) {
                perror("[CAPTAIN FERRY] msgsnd departure message failed");
            } else {
                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY %d] Sent departure notification to port\n", ferry_id);
            }

            // symulacja podrozy
            if (onboard > 0) {  // tylko jesli sa pasazerowie
                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY %d] Traveling with %d passengers (estimated %d seconds)...\n",
                    ferry_id, onboard, TRAVEL_TIME);

                sleep(TRAVEL_TIME);  // symulacja

                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY %d] Arrived at destination, passengers disembarking...\n",
                    ferry_id);

                // zmniejsz passengers_onboard
                sem_down(mutex, 0);
                port_state->passengers_onboard -= onboard;
                int remaining = port_state->passengers_onboard;
                my_ferry->status = FERRY_AVAILABLE;  // prom znowu dostepny
                // jesli sa czekajacy pasazerowie, otworz boarding od razu
                int wait_count = my_ferry->in_waiting + my_ferry->in_waiting_vip;
                if (wait_count > 0) {
                    my_ferry->boarding_allowed = true;
                    my_ferry->status = FERRY_BOARDING;
                    dprintf(STDOUT_FILENO,
                        "[CAPTAIN FERRY %d] %d passengers waiting, reopening boarding\n",
                        ferry_id, wait_count);
                }
                sem_up(mutex, 0);

                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY %d] Returned to port. %d passengers still onboard (all ferries).\n",
                    ferry_id, remaining);
            }
            
            // sprawdzanie warunku zakonczenia
            sem_down(mutex, 0);
            accepting = port_state->accepting_passengers;
            passengers_left = port_state->passengers_onboard;
            int still_waiting = my_ferry->in_waiting + my_ferry->in_waiting_vip;
            sem_up(mutex, 0);

            if (!accepting && passengers_left == 0 && still_waiting == 0) {
                dprintf(STDOUT_FILENO,
                    "[CAPTAIN FERRY %d] Port closed, no passengers remaining or waiting. Shutting down.\n",
                    ferry_id);

                sem_down(mutex, 0);
                my_ferry->status = FERRY_SHUTDOWN;
                sem_up(mutex, 0);
                break;
            }
        }
    }

    shmdt(port_state);
}
