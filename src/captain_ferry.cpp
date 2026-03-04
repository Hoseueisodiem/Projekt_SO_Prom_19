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
#include "log_uring.h"

// io_uring ring dla async logowania w tym procesie
static struct io_uring g_ring;
static bool g_ring_ok = false;

static void LOG(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (g_ring_ok) log_uring_printf(&g_ring, STDOUT_FILENO, fmt, ap);
    else           vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);
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
    if (log_uring_init(&g_ring, 32) == 0) g_ring_ok = true;

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
    spinlock_lock(port_state->spinlock);
    my_ferry->captain_pid = getpid();
    spinlock_unlock(port_state->spinlock);

    LOG( "[CAPTAIN FERRY %d] PID=%d, capacity=%d\n",
            ferry_id, getpid(), my_ferry->capacity);

    time_t last_departure = time(nullptr);
    
    while (true) {
        sleep(1);

        time_t now = time(nullptr);
        bool time_to_depart = (now - last_departure >= DEPARTURE_TIME);

        if (time_to_depart || early_departure) {
            // sprawdz czy prom jest pelny
            spinlock_lock(port_state->spinlock);
            bool ferry_full = (my_ferry->onboard >= my_ferry->capacity);
            spinlock_unlock(port_state->spinlock);

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
                    LOG(
                        "[CAPTAIN FERRY %d] Trap occupied, skipping departure\n", ferry_id);
                    if (early_departure) {
                        early_departure = 0;
                    }
                    continue;
                }
            } else {
                LOG(
                    "[CAPTAIN FERRY %d] Ferry full (%d/%d), departing regardless of trap state\n",
                    ferry_id, my_ferry->onboard, my_ferry->capacity);
            }

            // trap pusty lub prom pelny, kontynuacja
            
            // sprawdz czy sa pasazerowie
            spinlock_lock(port_state->spinlock);
            int accepting = port_state->accepting_passengers;
            int onboard = my_ferry->onboard;
            int in_waiting = my_ferry->in_waiting;
            int passengers_left = port_state->passengers_onboard.load(); // wait-free read
            spinlock_unlock(port_state->spinlock);

            // jesli port zamkniety i brak pasazerow (na promach + w poczekalni), koncz
            int waiting_for_me = my_ferry->in_waiting + my_ferry->in_waiting_vip;
            if (!accepting && passengers_left == 0 && onboard == 0 && waiting_for_me == 0) {
                LOG(
                    "[CAPTAIN FERRY %d] Port closed, no passengers anywhere. Shutting down.\n", ferry_id);

                spinlock_lock(port_state->spinlock);
                my_ferry->status = FERRY_SHUTDOWN;
                spinlock_unlock(port_state->spinlock);
                break;
            }

            // jesli brak pasazerow na promie
            if (onboard == 0) {
                if (early_departure) {
                    LOG(
                        "[CAPTAIN FERRY %d] Early departure signal received, but no passengers onboard yet (%d still in waiting). Waiting...\n",
                        ferry_id, in_waiting);

                    // poczekanie max 5s na pasazerow
                    bool passengers_arrived = false;
                    for (int wait_time = 0; wait_time < 10; wait_time++) {  // 5s
                        usleep(500000);  // 500ms

                        spinlock_lock(port_state->spinlock);
                        onboard = my_ferry->onboard;
                        spinlock_unlock(port_state->spinlock);

                        if (onboard > 0) {
                            LOG(
                                "[CAPTAIN FERRY %d] Passengers arrived (%d onboard), ready to depart\n",
                                ferry_id, onboard);
                            passengers_arrived = true;
                            break;
                        }
                    }

                    if (!passengers_arrived) {
                        LOG(
                            "[CAPTAIN FERRY %d] Still no passengers after waiting. Canceling early departure.\n",
                            ferry_id);
                        early_departure = 0;
                        continue;
                    }
                } else {
                    LOG(
                        "[CAPTAIN FERRY %d] Departure time reached, but no passengers. Skipping departure.\n",
                        ferry_id);
                    last_departure = now;
                    continue;
                }
            }

            // ustaw status TRAVELING
            spinlock_lock(port_state->spinlock);
            onboard = my_ferry->onboard;
            my_ferry->onboard = 0;
            my_ferry->status = FERRY_TRAVELING;
            spinlock_unlock(port_state->spinlock);

            LOG(
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
                LOG(
                    "[CAPTAIN FERRY %d] Sent departure notification to port\n", ferry_id);
            }

            // symulacja podrozy
            if (onboard > 0) {  // tylko jesli sa pasazerowie
                LOG(
                    "[CAPTAIN FERRY %d] Traveling with %d passengers (estimated %d seconds)...\n",
                    ferry_id, onboard, TRAVEL_TIME);

                sleep(TRAVEL_TIME);  // symulacja

                LOG(
                    "[CAPTAIN FERRY %d] Arrived at destination, passengers disembarking...\n",
                    ferry_id);

                // zmniejsz passengers_onboard
                int remaining = port_state->passengers_onboard.fetch_sub(onboard) - onboard;
                spinlock_lock(port_state->spinlock);
                my_ferry->status = FERRY_AVAILABLE;  // prom znowu dostepny
                // jesli sa czekajacy pasazerowie, otworz boarding od razu
                int wait_count = my_ferry->in_waiting + my_ferry->in_waiting_vip;
                if (wait_count > 0) {
                    my_ferry->boarding_allowed = true;
                    my_ferry->status = FERRY_BOARDING;
                    LOG(
                        "[CAPTAIN FERRY %d] %d passengers waiting, reopening boarding\n",
                        ferry_id, wait_count);
                }
                spinlock_unlock(port_state->spinlock);

                LOG(
                    "[CAPTAIN FERRY %d] Returned to port. %d passengers still onboard (all ferries).\n",
                    ferry_id, remaining);
            }
            
            // sprawdzanie warunku zakonczenia
            spinlock_lock(port_state->spinlock);
            accepting = port_state->accepting_passengers;
            passengers_left = port_state->passengers_onboard.load(); // wait-free read
            int still_waiting = my_ferry->in_waiting + my_ferry->in_waiting_vip;
            spinlock_unlock(port_state->spinlock);

            if (!accepting && passengers_left == 0 && still_waiting == 0) {
                LOG(
                    "[CAPTAIN FERRY %d] Port closed, no passengers remaining or waiting. Shutting down.\n",
                    ferry_id);

                spinlock_lock(port_state->spinlock);
                my_ferry->status = FERRY_SHUTDOWN;
                spinlock_unlock(port_state->spinlock);
                break;
            }
        }
    }

    shmdt(port_state);
    if (g_ring_ok) { log_uring_flush(&g_ring); log_uring_destroy(&g_ring); }
}
