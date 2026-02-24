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

/*=== kontrola bezpieczenstwa (stanowisko aktywnie sprawdza pasazera) ===*/
    // Wybor stanowiska: kobiety 0, mezczyznic1 lub 2
    int station;
    if (gender == FEMALE) {
        station = 0;
    } else {
        station = (id % 2 == 0) ? 1 : 2;
    }

    // poinformuj PortState ze pasazer wchodzi do kontroli
    key_t mutex_key = ftok("/tmp", 'X');
    int mutex = semget(mutex_key, 1, 0666);

    sem_down(mutex, 0);
    port_state->passengers_in_security++;
    sem_up(mutex, 0);

    // wyslij zgloszenie do stanowiska kontroli
    key_t sec_key = ftok("/tmp", 'Q');
    int sec_queue = msgget(sec_key, 0666);
    if (sec_queue == -1) {
        perror("[PASSENGER] msgget sec_queue");
        _exit(1);
    }

    SecurityJoinMsg join_msg;
    join_msg.mtype        = station + 1;  // mtype 1,2 lub 3
    join_msg.passenger_id = id;
    join_msg.gender       = gender;
    join_msg.vip          = vip ? 1 : 0;

    msgsnd(sec_queue, &join_msg, sizeof(SecurityJoinMsg) - sizeof(long), 0);

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d WAITING for security station %d\n",
            id, station);

    // czekaj az stanowisko zakonczy inspekcje
    SecurityDoneMsg done_msg;
    msgrcv(sec_queue, &done_msg,
           sizeof(SecurityDoneMsg) - sizeof(long),
           MSG_TYPE_SECURITY_DONE_BASE + id, 0);

    if (done_msg.dangerous_item_found) {
        dprintf(STDOUT_FILENO,
                "[PASSENGER] id=%d Dangerous item confiscated at station %d, "
                "passenger continues\n",
                id, station);
    } else {
        dprintf(STDOUT_FILENO,
                "[PASSENGER] id=%d Security check OK at station %d\n",
                id, station);
    }

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

/*=== czekanie na ogloszenie boardingu przez kapitana portu ===*/
    while (true) {
        sem_down(mutex, 0);
        bool boarding_open = my_ferry->boarding_allowed;
        bool port_open = port_state->accepting_passengers;
        FerryStatus fstatus = my_ferry->status;
        sem_up(mutex, 0);

        if (boarding_open) {
            dprintf(STDOUT_FILENO,
                    "[PASSENGER] id=%d Boarding announced for ferry %d, proceeding to gangway\n",
                    id, assigned_ferry_id);
            break;
        }
        if (fstatus == FERRY_SHUTDOWN) {
            dprintf(STDOUT_FILENO,
                    "[PASSENGER] id=%d Ferry shutdown while waiting for boarding, leaving\n", id);
            sem_down(mutex, 0);
            if (vip) my_ferry->in_waiting_vip--;
            else my_ferry->in_waiting--;
            sem_up(mutex, 0);
            shmdt(port_state);
            return;
        }
        sleep(1);
    }

/*=== poczekalnia -> trap -> prom + VIP prio ===*/
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
        FerryStatus bstatus = my_ferry->status;
        bool bport_open = port_state->accepting_passengers;

        bool can_enter = false;
        int free_spots = my_ferry->capacity - current_onboard;
        if (vip) {
            can_enter = ferry_has_space;
        } else {
            // regularny moze wejsc gdy: brak VIP-ow LUB jest wiecej miejsc niz VIP-ow
            can_enter = ferry_has_space && (vip_count == 0 || free_spots > vip_count);
        }
        sem_up(mutex, 0);

        // wyjscie tylko gdy prom shutdown (jesli odplynal - czekaj na powrot)
        if (bstatus == FERRY_SHUTDOWN) {
            dprintf(STDOUT_FILENO,
                    "[PASSENGER] id=%d Ferry %d shutdown, leaving\n",
                    id, assigned_ferry_id);
            sem_down(mutex, 0);
            if (vip) my_ferry->in_waiting_vip--;
            else my_ferry->in_waiting--;
            sem_up(mutex, 0);
            shmdt(port_state);
            return;
        }

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
}