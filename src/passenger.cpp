#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/prctl.h>

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
    prctl(PR_SET_NAME, "pasazer");

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
    bool has_dangerous_item = (rand() % 100 < DANGEROUS_ITEM_CHANCE);

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d %s gender=%s%s\n",
            id,
            vip ? "VIP" : "REGULAR",
            gender == MALE ? "MALE" : "FEMALE",
            has_dangerous_item ? " [HAS DANGEROUS ITEM]" : "");

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
        if (decision.reject_reason == REJECT_PORT_CLOSED) {
            dprintf(STDOUT_FILENO, "[PASSENGER] id=%d REJECTED (port closed)\n", id);
        } else {
            dprintf(STDOUT_FILENO, "[PASSENGER] id=%d REJECTED (baggage too heavy)\n", id);
        }
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

    int station;
    if (gender == FEMALE) {
        station = 0;
    } else {
        station = (id % 2 == 0) ? 1 : 2;  // mezczyzni na stanowiska 1 lub 2
    }

    dprintf(STDOUT_FILENO, "[PASSENGER] id=%d WAITING for security station %d\n", id, station);

    // inicjalizuj licznik przed petla
    sem_down(mutex, 0);
    int last_total_entered = stations[station].total_entered;
    sem_up(mutex, 0);

    int skipped_count = 0;  // ile osob mnie przepuscilo
    bool has_priority = false;  // czy ma priorytet po 3 przepuszczeniach
    int security_wait_iter = 0;

    while (true) {
        security_wait_iter++;
        sem_down(mutex, 0);

        // Sprawdz czy port wciaz otwarty (w kolejce do kontroli)
        if (!port_state->accepting_passengers) {
            sem_up(mutex, 0);
            dprintf(STDOUT_FILENO,
                "[PASSENGER] id=%d Port closed while waiting for security, leaving\n", id);
            shmdt(port_state);
            shmdt(stations);
            return;
        }

        // sprawdz ile osob weszlo od ostatniego sprawdzenia
        int new_entries = stations[station].total_entered - last_total_entered;
        if (new_entries > 0 && !has_priority) {
            skipped_count += new_entries;  // ktos mnie przepuscil
            last_total_entered = stations[station].total_entered;

            if (!vip && new_entries > 0 && security_wait_iter % 5 == 1) {
                dprintf(STDOUT_FILENO,
                "[PASSENGER] id=%d skipped by %d passenger(s), total skipped: %d/3\n",
                id, new_entries, skipped_count);
            }

            // po 3 przepuszczeniach dostaje prio
            if (!vip && skipped_count >= 3 && !has_priority) {
                has_priority = true;
                stations[station].priority_waiting++;
                dprintf(STDOUT_FILENO,
                        "[PASSENGER] id=%d gained PRIORITY after being skipped %d times (priority queue: %d)\n",
                        id, skipped_count, stations[station].priority_waiting);
            }
        }

        // logika wejscia
        bool can_enter = false;

        if (has_priority) {
            if (stations[station].count == 0) {
                stations[station].gender = gender;
                can_enter = true;
            } else if (stations[station].gender == gender && stations[station].count < 2) {
                can_enter = true;
            }
        } else {
            // normalny pasazer moze wejsc tylko gdy nikt z priorytetem nie czeka
            if (stations[station].priority_waiting == 0) {
                if (stations[station].count == 0) {
                    stations[station].gender = gender;
                }
                if (stations[station].gender == gender && stations[station].count < 2) {
                    can_enter = true;
                }
            }
        }

        if (can_enter) {
            stations[station].count++;
            stations[station].total_entered++;

            // jesli mial prio zmniejsz licznik priorytetowych
            if (has_priority) {
                stations[station].priority_waiting--;
            }

            sem_up(mutex, 0);

            dprintf(STDOUT_FILENO, "[PASSENGER] id=%d %sENTER security station %d\n",
                    id, has_priority ? "(PRIORITY) " : "", station);
            break;
        }

        sem_up(mutex, 0);
        sleep(1);
    }

    usleep(100000); // symulacja kontroli (100ms)
    if (has_dangerous_item) {
        has_dangerous_item = false;
        dprintf(STDOUT_FILENO,
                "[SECURITY] id=%d DANGEROUS ITEM FOUND at station %d! Item confiscated, passenger continues.\n",
                id, station);
        usleep(100000); // dodatkowy czas na konfiskate (100ms)
    } else {
        dprintf(STDOUT_FILENO,
                "[SECURITY] id=%d OK at station %d\n",
                id, station);
    }

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d LEAVE security station %d\n",
            id, station);

    //wyjscie
    sem_down(mutex, 0);
    stations[station].count--;
    if (stations[station].count == 0) {
        stations[station].gender = -1;
    }

    // Sprawdz czy port wciaz otwarty PO kontroli
    if (!port_state->accepting_passengers) {
        sem_up(mutex, 0);
        dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d Port closed after security, cannot board. Leaving.\n", id);
        shmdt(port_state);
        shmdt(stations);
        return;
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
    int trap_sem = semget(trap_key, NUM_FERRIES, 0666);

    int boarding_wait_iter = 0;
    while (true) {
        boarding_wait_iter++;
        sem_down(mutex, 0);
        bool ferry_has_space = (my_ferry->onboard < my_ferry->capacity);
        int current_onboard = my_ferry->onboard;
        int vip_count = my_ferry->in_waiting_vip;

        bool can_enter = false;
        if (vip) {
            can_enter = ferry_has_space;
        } else {
            can_enter = ferry_has_space && (vip_count == 0);
        }
        sem_up(mutex, 0);

        if (!can_enter) {
            if (boarding_wait_iter % 5 == 1) {
                if (!ferry_has_space) {
                    dprintf(STDOUT_FILENO, "[PASSENGER] id=%d Waiting, ferry %d full (%d/%d)\n",
                        id, assigned_ferry_id, current_onboard, my_ferry->capacity);
                } else {
                    dprintf(STDOUT_FILENO, "[PASSENGER] id=%d Waiting for %d VIP passengers to board first\n",
                        id, vip_count);
                }
            }

            sleep(1);
            continue;
        }

        // prom ma miejsce, sprobuj wejsc na trap
        struct sembuf sb = {static_cast<unsigned short>(assigned_ferry_id), -1, IPC_NOWAIT};
        if (semop(trap_sem, &sb, 1) != -1) {
            // zdobyty trap, ale sprawdz jeszcze raz pojemnosc
            sem_down(mutex, 0);
            
            // triple-check pojemnosci
            if (my_ferry->onboard < my_ferry->capacity) {
                // BOARDING
                if (vip) my_ferry->in_waiting_vip--;
                else my_ferry->in_waiting--;

                my_ferry->onboard++;
                port_state->passengers_onboard++;

                int final_onboard = my_ferry->onboard;
                int final_waiting = my_ferry->in_waiting;
                int final_vip_waiting = my_ferry->in_waiting_vip;
                int total_onboard = port_state->passengers_onboard;

                sem_up(mutex, 0);
                sem_up(trap_sem, assigned_ferry_id);

                dprintf(STDOUT_FILENO,
                        "[PASSENGER] id=%d %sBOARDED ferry %d (%d reg, %d VIP waiting, %d/%d onboard, %d total)\n",
                        id, vip ? "(VIP) " : "", assigned_ferry_id,
                        final_waiting, final_vip_waiting,
                        final_onboard, my_ferry->capacity,
                        total_onboard);

                break;
            } else {
                // prom zapelnil sie w miedzyczasie
                sem_up(mutex, 0);
                sem_up(trap_sem, assigned_ferry_id);
                dprintf(STDOUT_FILENO,
                        "[PASSENGER] id=%d Ferry became full while on gangway\n", id);
                sleep(1);
                continue;
            }
        }

        // nie udalo sie wejsc na trap czekaj
        sleep(1);
    }

    //cleanup
    shmdt(port_state);
    shmdt(stations);
}