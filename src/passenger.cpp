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

    // czy port przyjmuje pasazerow
    key_t port_state_key = ftok("/tmp", 'S');
    int port_shmid = shmget(port_state_key, sizeof(PortState), 0666);
    
    // retry jak port_state jeszcze nie istnieje
    for (int attempt = 0; attempt < 50 && port_shmid == -1; attempt++) {
        usleep(100000);
        port_shmid = shmget(port_state_key, sizeof(PortState), 0666);
    }
    
    if (port_shmid == -1) {
        dprintf(STDERR_FILENO, "[PASSENGER %d] Failed to access port state\n", id);
        perror("[PASSENGER] shmget port_state");
        _exit(1);
    }
    
    PortState* port_state = (PortState*) shmat(port_shmid, nullptr, 0);
    if (port_state == (void*) -1) {
        perror("[PASSENGER] shmat port_state");
        _exit(1);
    }
    
    // spradzanie czy port przyjmuje pasazerow
    if (!port_state->accepting_passengers) {
        dprintf(STDOUT_FILENO, "[PASSENGER] id=%d Port CLOSED, cannot enter\n", id);
        shmdt(port_state);
        return;  // pasazer odchodzi
    }

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
        shmdt(port_state); //cleanup
        return;
    }

    // zapamietaj ID przydzielonego promu
    int assigned_ferry_id = decision.ferry_id;

    dprintf(STDOUT_FILENO, "[PASSENGER] id=%d ACCEPTED (assigned to ferry %d)\n",
            id, assigned_ferry_id);

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
                shmdt(port_state);
                shmdt(stations);
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

    // uzyj przydzielonego promu z PortState
    Ferry* my_ferry = &port_state->ferries[assigned_ferry_id];

    sem_down(mutex, 0);
    if (vip) {
        my_ferry->in_waiting_vip++;
        int vip_waiting = my_ferry->in_waiting_vip;
        sem_up(mutex, 0);
        dprintf(STDOUT_FILENO,
                "[PASSENGER] id=%d (VIP) ENTERED waiting area for ferry %d (%d VIP waiting)\n",
                id, assigned_ferry_id, vip_waiting);
    } else {
        my_ferry->in_waiting++;
        int reg_waiting = my_ferry->in_waiting;
        sem_up(mutex, 0);
        dprintf(STDOUT_FILENO,
                "[PASSENGER] id=%d ENTERED waiting area for ferry %d (%d regular waiting)\n",
                id, assigned_ferry_id, reg_waiting);
    }

/*=== poczekalnia -> trap -> prom + frustracja + VIP prio ===*/
    // trap
    key_t trap_key = ftok("/tmp", 'T');
    int trap_sem = semget(trap_key, 1, 0666);

    while (true) {
        sem_down(mutex, 0);
        bool ferry_has_space = (my_ferry->onboard < my_ferry->capacity);
        int current_onboard = my_ferry->onboard;
        int vip_count = my_ferry->in_waiting_vip;

        // VIP moze wejsc jesli prom ma miejsce
        // regularny moze wejsc tylko jesli prom ma miejsce i brak VIP w kolejce
        bool can_enter = false;
        if (vip) {
            can_enter = ferry_has_space;
        } else {
            can_enter = ferry_has_space && (vip_count == 0);
        }
        sem_up(mutex, 0);

        if (!can_enter) {
            if (!ferry_has_space) {
                dprintf(STDOUT_FILENO, "[PASSENGER] id=%d Waiting, ferry %d full (%d/%d)\n",
                    id, assigned_ferry_id, current_onboard, my_ferry->capacity);
            } else {
                // regularny czeka na VIP
                dprintf(STDOUT_FILENO, "[PASSENGER] id=%d Waiting for %d VIP passengers to board first\n",
                    id, vip_count);
            }

            if (!vip) {
                waited++;
                if (waited > 3) {
                    dprintf(STDOUT_FILENO,
                        "[PASSENGER] id=%d FRUSTRATED, ferry full or waiting for VIP too long\n", id);
                    sem_down(mutex, 0);
                    my_ferry->in_waiting--;
                    sem_up(mutex, 0);

                    shmdt(port_state);
                    shmdt(stations);
                    return;
                }
            }
            sleep(1);
            continue;
        }

        // prom ma miejsce, sprobuj wejsc na trap
        struct sembuf sb = {0, -1, IPC_NOWAIT};
        if (semop(trap_sem, &sb, 1) != -1) {
            // zdobyty trap, ale sprawdz jeszcze raz pojemnosc
            sem_down(mutex, 0);
            bool still_has_space = (my_ferry->onboard < my_ferry->capacity);
            sem_up(mutex, 0);

            if (still_has_space) {
                break;
            } else {
                // prom zapelnil sie w miedzyczasie
                sem_up(trap_sem, 0);
                dprintf(STDOUT_FILENO, "[PASSENGER] id=%d Ferry became full while on gangway, releasing\n", id);

                if (!vip) {
                    waited++;
                    if (waited > 3) {
                        dprintf(STDOUT_FILENO,
                            "[PASSENGER] id=%d FRUSTRATED, ferry full too long\n", id);
                        sem_down(mutex, 0);
                        if (vip) my_ferry->in_waiting_vip--;
                        else my_ferry->in_waiting--;
                        sem_up(mutex, 0);

                        shmdt(port_state);
                        shmdt(stations);
                        return;
                    }
                }
                sleep(1);
                continue;
            }
        }

        if (!vip) {
            waited++;
            if (waited > 3) {
                dprintf(STDOUT_FILENO,
                    "[PASSENGER] id=%d FRUSTRATED, gangway queue too long\n", id);
                sem_down(mutex, 0);
                my_ferry->in_waiting--;
                sem_up(mutex, 0);

                shmdt(port_state);
                shmdt(stations);
                return;
            }
        }
        sleep(1);
    }

    // wyjscie z wlasciwej kolejki (VIP lub regularnej)
    sem_down(mutex, 0);
    if (vip) my_ferry->in_waiting_vip--;
    else my_ferry->in_waiting--;

    if (my_ferry->onboard < my_ferry->capacity) {
        my_ferry->onboard++;
        port_state->passengers_onboard++;
        dprintf(STDOUT_FILENO, "[PASSENGER] id=%d %sBOARDED ferry %d (%d reg, %d VIP waiting, %d/%d onboard, %d total)\n",
                id, vip ? "(VIP) " : "", assigned_ferry_id,
                my_ferry->in_waiting, my_ferry->in_waiting_vip,
                my_ferry->onboard, my_ferry->capacity,
                port_state->passengers_onboard);
    } else {
        // prom zapelnij sie w miedzyczasie
        if (vip) my_ferry->in_waiting_vip++;
        else my_ferry->in_waiting++;
        dprintf(STDOUT_FILENO, "[PASSENGER] id=%d Ferry %d became full, back to waiting\n",
                id, assigned_ferry_id);
    }
    sem_up(mutex, 0);

    sem_up(trap_sem, 0);

    //cleanup
    shmdt(port_state);
    shmdt(stations);
}