#include <cstdio>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <signal.h>
#include <climits>
#include <vector>
#include <sys/prctl.h>
#include "captain_port.h"
#include "ipc.h"
#include "security.h"

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

volatile sig_atomic_t port_closed = 0;

static void handle_sigusr2(int) {
    port_closed = 1;
}

static void cleanup_ipc() {
    // usun kolejkę komunikatów
    key_t msg_key = ftok("/tmp", 'P');
    if (msg_key != -1) {
        int msgid = msgget(msg_key, 0);
        if (msgid != -1) {
            msgctl(msgid, IPC_RMID, nullptr);
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Removed old message queue\n");
        }
    }
    
    // usun pamięć stanowisk
    key_t shm_key = ftok("/tmp", 'M');
    if (shm_key != -1) {
        int shmid = shmget(shm_key, 0, 0);
        if (shmid != -1) {
            shmctl(shmid, IPC_RMID, nullptr);
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Removed old stations shared memory\n");
        }
    }

    // usun mutex
    key_t mutex_key = ftok("/tmp", 'X');
    if (mutex_key != -1) {
        int semid = semget(mutex_key, 0, 0);
        if (semid != -1) {
            semctl(semid, 0, IPC_RMID);
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Removed old mutex\n");
        }
    }
    
    // usun trap semafora
    key_t trap_key = ftok("/tmp", 'T');
    if (trap_key != -1) {
        int semid = semget(trap_key, 0, 0);
        if (semid != -1) {
            semctl(semid, 0, IPC_RMID);
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Removed old trap semaphore\n");
        }
    }

    // usun port state
    key_t port_state_key = ftok("/tmp", 'S');
    if (port_state_key != -1) {
        int shmid = shmget(port_state_key, 0, 0);
        if (shmid != -1) {
            shmctl(shmid, IPC_RMID, nullptr);
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Removed old port state shared memory\n");
        }
    }
}

// przydzielenie pasazera do promu
int assign_passenger_to_ferry(PortState* port_state, int mutex, int baggage_weight) {
    sem_down(mutex, 0);

    int selected_ferry = -1;
    int min_queue = INT_MAX;  // najmniejsza kolejka

    // znajdz pierwszy dostepny prom z wolnym miejscem
    for (int i = 0; i < NUM_FERRIES; i++) {
        Ferry* ferry = &port_state->ferries[i];

        // sprawdz czy bagaz pasuje do limitu tego promu
        if (baggage_weight > ferry->baggage_limit) {
            continue;  // pomin ten prom
        }

        // prom musi byc dostepny lub w trakcie zaladunku
        if (ferry->status == FERRY_AVAILABLE || ferry->status == FERRY_BOARDING) {
            int total = ferry->in_waiting + ferry->onboard;

            if (total < ferry->capacity) {
                // znajdz prom z najmniejsza kolejka
                if (total < min_queue) {
                    min_queue = total;
                    selected_ferry = i;
                }
            }
        }
    }

    // ustaw status wybranego promu
    if (selected_ferry != -1) {
        port_state->ferries[selected_ferry].status = FERRY_BOARDING;
        if (!port_state->ferries[selected_ferry].boarding_allowed) {
            port_state->ferries[selected_ferry].boarding_allowed = true;
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Ferry %d BOARDING OPEN\n", selected_ferry);
        }
    }

    sem_up(mutex, 0);
    return selected_ferry;
}

// kolejka oczekujacych pasazerow (gdy wszystkie promy pelne/w podrozy)
static void process_waiting_queue(std::vector<PassengerMessage>& waiting_queue,
                                   PortState* port_state, int mutex, int msgid) {
    if (waiting_queue.empty()) return;

    // probuj przydzielic pasazerow z kolejki do promow
    std::vector<PassengerMessage> still_waiting;

    for (auto& pmsg : waiting_queue) {
        int ferry_id = assign_passenger_to_ferry(port_state, mutex, pmsg.baggage_weight);

        if (ferry_id != -1) {
            DecisionMessage decision;
            decision.mtype = MSG_TYPE_DECISION_BASE + pmsg.passenger_id;
            decision.passenger_id = pmsg.passenger_id;
            decision.accepted = 1;
            decision.ferry_id = ferry_id;

            dprintf(STDOUT_FILENO,
                    "[CAPTAIN PORT] Passenger id=%d FROM QUEUE assigned to ferry %d (current: %d waiting, %d onboard, capacity: %d)\n",
                    pmsg.passenger_id, ferry_id,
                    port_state->ferries[ferry_id].in_waiting,
                    port_state->ferries[ferry_id].onboard,
                    port_state->ferries[ferry_id].capacity);

            msgsnd(msgid, &decision, sizeof(DecisionMessage) - sizeof(long), 0);
        } else {
            still_waiting.push_back(pmsg);
        }
    }

    waiting_queue = still_waiting;

}

void run_captain_port() {
    prctl(PR_SET_NAME, "kapitan_port");
    signal(SIGUSR2, handle_sigusr2);
    cleanup_ipc();

    key_t key = ftok("/tmp", 'P');
    int msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN PORT] failed to create message queue\n");
        _exit(1);
    }

    // pamiec wspoldzielona stan stanowisk
    key_t shm_key = ftok("/tmp", 'M');
    int shmid = shmget(shm_key, sizeof(SecurityStation) * NUM_STATIONS, IPC_CREAT | 0666);
    if (shmid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN PORT] failed to create shared memory\n");
        _exit(1);
    }

    SecurityStation* stations = (SecurityStation*) shmat(shmid, nullptr, 0);
    if (stations == (void*) -1) {
        perror("shmat stations");
        _exit(1);
    }

    for (int i = 0; i < NUM_STATIONS; i++) {
        stations[i].count = 0;
        stations[i].gender = -1;
        stations[i].total_entered = 0;
        stations[i].priority_waiting = 0;
    }

    // mutex do ochrony stanu
    key_t mutex_key = ftok("/tmp", 'X');
    int mutex = semget(mutex_key, 1, IPC_CREAT | 0666);
    semctl(mutex, 0, SETVAL, 1);

    // trap K
    key_t trap_key = ftok("/tmp", 'T');
    int trap_sem = semget(trap_key, NUM_FERRIES, IPC_CREAT | 0666);
    for (int i = 0; i < NUM_FERRIES; i++) {
        semctl(trap_sem, i, SETVAL, GANGWAY_CAPACITY);
    }

    // port state (stare ferry)
    key_t port_state_key = ftok("/tmp", 'S');
    int port_shmid = shmget(port_state_key, sizeof(PortState), IPC_CREAT | 0666);
    if (port_shmid == -1) {
        perror("[CAPTAIN PORT] shmget port_state");
        _exit(1);
    }

    PortState* port_state = (PortState*) shmat(port_shmid, nullptr, 0);
    if (port_state == (void*) -1) {
        perror("[CAPTAIN PORT] shmat port_state");
        _exit(1);
    }

    // inicjalizacja stanu portu i promow
    port_state->accepting_passengers = 1;
    port_state->passengers_onboard = 0;

    // inicjalizuj kazdy prom
    for (int i = 0; i < NUM_FERRIES; i++) {
        port_state->ferries[i].onboard = 0;
        port_state->ferries[i].in_waiting = 0;
        port_state->ferries[i].in_waiting_vip = 0;
        port_state->ferries[i].capacity = FERRY_CAPACITY;
        port_state->ferries[i].baggage_limit = MAX_BAGGAGE;
        port_state->ferries[i].status = FERRY_AVAILABLE;
        port_state->ferries[i].captain_pid = -1;
        port_state->ferries[i].signal_sent = false;
        port_state->ferries[i].boarding_allowed = false;
    }

    port_state->ferries[0].baggage_limit = 25;  // Prom 0: xkg
    port_state->ferries[1].baggage_limit = 20;  // Prom 1: xkg
    port_state->ferries[2].baggage_limit = 30;  // Prom 2: xkg

    dprintf(STDOUT_FILENO, "[CAPTAIN PORT] PID=%d waiting for passengers...\n", getpid());
    dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Initialized %d ferries:\n", NUM_FERRIES);
    dprintf(STDOUT_FILENO, "  - Ferry 0: capacity=%d, baggage_limit=%dkg\n", 
        FERRY_CAPACITY, port_state->ferries[0].baggage_limit);
    dprintf(STDOUT_FILENO, "  - Ferry 1: capacity=%d, baggage_limit=%dkg\n", 
        FERRY_CAPACITY, port_state->ferries[1].baggage_limit);
    dprintf(STDOUT_FILENO, "  - Ferry 2: capacity=%d, baggage_limit=%dkg\n", 
        FERRY_CAPACITY, port_state->ferries[2].baggage_limit);

    int accepted_count = 0;
    std::vector<PassengerMessage> waiting_queue;

    while (true) {
        // sprawdzanie czy port zamkniety
        if (port_closed) {
            port_state->accepting_passengers = 0;
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] SIGUSR2 received - Port CLOSED. No more passengers allowed.\n");

            // przetworz reszte kolejki - przydziel kogo sie da, reszta odrzucona
            process_waiting_queue(waiting_queue, port_state, mutex, msgid);

            // odrzuc pozostalych w kolejce
            for (auto& pmsg : waiting_queue) {
                DecisionMessage decision;
                decision.mtype = MSG_TYPE_DECISION_BASE + pmsg.passenger_id;
                decision.passenger_id = pmsg.passenger_id;
                decision.accepted = 0;
                decision.ferry_id = -1;
                decision.reject_reason = REJECT_PORT_CLOSED;
                msgsnd(msgid, &decision, sizeof(DecisionMessage) - sizeof(long), 0);
                dprintf(STDOUT_FILENO,
                        "[CAPTAIN PORT] Passenger id=%d REJECTED from queue (port closing)\n",
                        pmsg.passenger_id);
            }
            waiting_queue.clear();

            // otworz boarding dla wszystkich promow z czekajacymi pasazerami
            sem_down(mutex, 0);
            for (int i = 0; i < NUM_FERRIES; i++) {
                int waiting = port_state->ferries[i].in_waiting + port_state->ferries[i].in_waiting_vip;
                if (waiting > 0 && !port_state->ferries[i].boarding_allowed) {
                    port_state->ferries[i].boarding_allowed = true;
                    port_state->ferries[i].status = FERRY_BOARDING;
                    dprintf(STDOUT_FILENO,
                        "[CAPTAIN PORT] Ferry %d BOARDING OPEN (port closing, %d passengers still waiting)\n",
                        i, waiting);
                }
            }
            sem_up(mutex, 0);

            // czekanie az wszyscy pasazerowie zostana przewiezieni (onboard + waiting)
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Waiting for all passengers to be delivered...\n");
            while (true) {
                // otworz boarding dla promow z nowymi pasazerami (moga dojsc z kontroli)
                sem_down(mutex, 0);
                for (int i = 0; i < NUM_FERRIES; i++) {
                    int waiting = port_state->ferries[i].in_waiting + port_state->ferries[i].in_waiting_vip;
                    if (waiting > 0 && !port_state->ferries[i].boarding_allowed &&
                        port_state->ferries[i].status != FERRY_TRAVELING) {
                        port_state->ferries[i].boarding_allowed = true;
                        port_state->ferries[i].status = FERRY_BOARDING;
                        dprintf(STDOUT_FILENO,
                            "[CAPTAIN PORT] Ferry %d BOARDING OPEN (closing, %d new passengers arrived)\n",
                            i, waiting);
                    }
                }
                int remaining = port_state->passengers_onboard;
                int total_waiting = 0;
                for (int i = 0; i < NUM_FERRIES; i++) {
                    total_waiting += port_state->ferries[i].in_waiting + port_state->ferries[i].in_waiting_vip;
                }
                sem_up(mutex, 0);

                if (remaining == 0 && total_waiting == 0) break;

                dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Still %d onboard, %d waiting. Waiting...\n", remaining, total_waiting);
                sleep(1);
            }

            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] All passengers delivered. Shutting down.\n");
            break;
        }

        // obsluga pasazerow z przydzielaniem do promu
        PassengerMessage msg;
        ssize_t msg_result = msgrcv(msgid, &msg,
                                     sizeof(PassengerMessage) - sizeof(long),
                                     MSG_TYPE_PASSENGER,
                                     IPC_NOWAIT);

        if (msg_result != -1) {
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Passenger id=%d baggage=%d kg\n",
                    msg.passenger_id, msg.baggage_weight);

            DecisionMessage decision;
            decision.mtype = MSG_TYPE_DECISION_BASE + msg.passenger_id;
            decision.passenger_id = msg.passenger_id;

            // przydziel pasazera do promu
            int ferry_id = assign_passenger_to_ferry(port_state, mutex, msg.baggage_weight);

            if (ferry_id == -1) {
                // sprawdz przyczyne odrzucenia
                bool baggage_too_heavy = true;
                for (int i = 0; i < NUM_FERRIES; i++) {
                    if (msg.baggage_weight <= port_state->ferries[i].baggage_limit) {
                        baggage_too_heavy = false;
                        break;
                    }
                }

                if (baggage_too_heavy) {
                    // bagaz za ciezki - odrzuc od razu
                    decision.accepted = 0;
                    decision.ferry_id = -1;
                    decision.reject_reason = REJECT_BAGGAGE;
                    dprintf(STDOUT_FILENO,
                            "[CAPTAIN PORT] Passenger id=%d REJECTED (baggage %dkg exceeds all ferry limits: max 30kg)\n",
                            msg.passenger_id, msg.baggage_weight);
                    msgsnd(msgid, &decision, sizeof(DecisionMessage) - sizeof(long), 0);
                } else {
                    // promy pelne/w podrozy - dodaj do kolejki, NIE wysylaj decision
                    waiting_queue.push_back(msg);
                    dprintf(STDOUT_FILENO,
                            "[CAPTAIN PORT] Passenger id=%d QUEUED (all suitable ferries full or traveling, queue size: %zu)\n",
                            msg.passenger_id, waiting_queue.size());
                }
                continue;
            } else {
                // zaakceptowany
                decision.accepted = 1;
                decision.ferry_id = ferry_id;
                accepted_count++;

                dprintf(STDOUT_FILENO,
                        "[CAPTAIN PORT] Passenger id=%d ACCEPTED, assigned to ferry %d (current: %d waiting, %d onboard, capacity: %d)\n",
                        msg.passenger_id, ferry_id,
                        port_state->ferries[ferry_id].in_waiting,
                        port_state->ferries[ferry_id].onboard,
                        port_state->ferries[ferry_id].capacity);
            }

            msgsnd(msgid, &decision, sizeof(DecisionMessage) - sizeof(long), 0);
            continue;
        }

        // obsluga komunikatow o odplynieciu promu
        FerryDepartureMessage departure_msg;
        if (msgrcv(msgid, &departure_msg,
                   sizeof(FerryDepartureMessage) - sizeof(long),
                   MSG_TYPE_FERRY_DEPARTED, IPC_NOWAIT) != -1) {

            dprintf(STDOUT_FILENO,
                "[CAPTAIN PORT] Ferry %d departed with %d passengers. Ready for next ferry.\n",
                departure_msg.ferry_id, departure_msg.passengers_count);

            // reset flagi sygnalu dla tego promu
            sem_down(mutex, 0);
            port_state->ferries[departure_msg.ferry_id].signal_sent = false;
            port_state->ferries[departure_msg.ferry_id].boarding_allowed = false;
            sem_up(mutex, 0);

            // prom odplynal - przetworz kolejke oczekujacych (promy wrocily = wolne miejsca)
            process_waiting_queue(waiting_queue, port_state, mutex, msgid);

            continue;
        }

        // SIGUSR1 dla kazdego promu osobno
        sem_down(mutex, 0);
        for (int i = 0; i < NUM_FERRIES; i++) {
            Ferry* ferry = &port_state->ferries[i];

            // pomin promy ktore nie sa dostepne lub juz otrzymaly sygnal
            if (ferry->status == FERRY_TRAVELING ||
                ferry->status == FERRY_SHUTDOWN ||
                ferry->signal_sent) {
                continue;
            }

            int total_ready = ferry->in_waiting + ferry->in_waiting_vip + ferry->onboard;

            // SIGUSR1 gdy >=50% zapelnienia
            if (total_ready >= ferry->capacity / 2 && ferry->captain_pid > 0) {
                sem_up(mutex, 0);  // zwolnienie mutexy przed killem

                dprintf(STDOUT_FILENO,
                    "[CAPTAIN PORT] Ferry %d half capacity (%d reg + %d VIP + %d onboard = %d/%d), sending EARLY DEPARTURE to PID=%d\n",
                    i, ferry->in_waiting, ferry->in_waiting_vip, ferry->onboard, total_ready, ferry->capacity, ferry->captain_pid);

                if (kill(ferry->captain_pid, SIGUSR1) == -1) {
                    perror("[CAPTAIN PORT] kill SIGUSR1 failed");
                } else {
                    dprintf(STDOUT_FILENO, "[CAPTAIN PORT] SIGUSR1 sent successfully to ferry %d\n", i);

                    sem_down(mutex, 0);
                    ferry->signal_sent = true;
                    sem_up(mutex, 0);
                }

                sem_down(mutex, 0);  // przywrocenie mutexy do petli
            }
        }
        sem_up(mutex, 0);

        // periodycznie przetworz kolejke oczekujacych
        process_waiting_queue(waiting_queue, port_state, mutex, msgid);

        usleep(100000);
    }
    dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Waiting for all ferries to shut down...\n");
    int wait_count = 0;
    while (true) {
        sem_down(mutex, 0);
        bool all_shutdown = true;
        for (int j = 0; j < NUM_FERRIES; j++) {
            if (port_state->ferries[j].status != FERRY_SHUTDOWN) {
                all_shutdown = false;
                break;
            }
        }
        sem_up(mutex, 0);

        if (all_shutdown) {
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] All ferries shut down.\n");
            break;
        }

        wait_count++;
        dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Waiting for ferries to shut down (%d)...\n", wait_count);
        sleep(1);
    }

    shmdt(port_state);
    shmdt(stations);
    cleanup_ipc();
}
