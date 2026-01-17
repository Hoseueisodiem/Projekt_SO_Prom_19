#include <cstdio>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <signal.h>
#include "captain_port.h"
#include "ipc.h"
#include "security.h"

#define MAX_BAGGAGE 20 // max dop waga kg
extern pid_t ferry_pid;

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
    
    // usun pamięć promu
    key_t ferry_key = ftok("/tmp", 'F');
    if (ferry_key != -1) {
        int shmid = shmget(ferry_key, 0, 0);
        if (shmid != -1) {
            shmctl(shmid, IPC_RMID, nullptr);
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Removed old ferry shared memory\n");
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
}

void run_captain_port() {
    cleanup_ipc();

    key_t key = ftok("/tmp", 'P');
    int msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid == -1) {
        dprintf(STDERR_FILENO, "[CAPTAIN PORT] failed to create message queue\n");
        _exit(1);
    }

    // pamiec wspoldzielona stan stanowisk
    key_t shm_key = ftok("/tmp", 'M');
    int shmid = shmget(shm_key,
                      sizeof(SecurityStation) * NUM_STATIONS, IPC_CREAT | 0666);
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
        stations[i].gender = -1; // brak
    }

    // mutex do ochrony stanu
    key_t mutex_key = ftok("/tmp", 'X');
    int mutex = semget(mutex_key, 1, IPC_CREAT | 0666);
    semctl(mutex, 0, SETVAL, 1);

    // trap K
    key_t trap_key = ftok("/tmp", 'T');
    int trap_sem = semget(trap_key, 1, IPC_CREAT | 0666);
    semctl(trap_sem, 0, SETVAL, GANGWAY_CAPACITY);

    // prom P
    key_t ferry_key = ftok("/tmp", 'F');
    int ferry_shmid = shmget(ferry_key, sizeof(FerryState), IPC_CREAT | 0666);
    FerryState* ferry = (FerryState*) shmat(ferry_shmid, nullptr, 0);

    if (ferry == (void*) -1) {
    perror("shmat ferry");
    _exit(1);
    }

    ferry->onboard = 0;
    ferry->in_waiting = 0;

    dprintf(STDOUT_FILENO, "[CAPTAIN PORT] PID=%d waiting for passengers...\n", getpid());

    int accepted_count = 0;
    int current_ferry_passengers = 0;
    bool signal_sent_for_current_ferry = false;

    while (true) {
        // sprawdzenie czy prom odplynal
        FerryDepartureMessage departure_msg;
        if (msgrcv(msgid, &departure_msg, 
                   sizeof(FerryDepartureMessage) - sizeof(long),
                   MSG_TYPE_FERRY_DEPARTED, IPC_NOWAIT) != -1) {
            
            dprintf(STDOUT_FILENO, 
                "[CAPTAIN PORT] Ferry departed with %d passengers. Ready for next ferry.\n",
                departure_msg.passengers_count);
            
            current_ferry_passengers = 0;
            signal_sent_for_current_ferry = false;
        }
        
        // sprawdzenie czy jest pasazer
        PassengerMessage msg;
        if (msgrcv(msgid, &msg, 
                   sizeof(PassengerMessage) - sizeof(long), 
                   MSG_TYPE_PASSENGER, IPC_NOWAIT) == -1) {
            
            // sprawdz czy wyslac sygnal na pods poczekalni
            if (!signal_sent_for_current_ferry) {
                sem_down(mutex, 0);
                int in_waiting = ferry->in_waiting;
                int onboard = ferry->onboard;
                int total_ready = in_waiting + onboard;
                sem_up(mutex, 0);
                
                if (total_ready >= FERRY_CAPACITY / 2) {
                    dprintf(STDOUT_FILENO,
                        "[CAPTAIN PORT] Half capacity reached (%d waiting + %d onboard = %d/%d), sending EARLY DEPARTURE signal to PID=%d\n",
                        in_waiting, onboard, total_ready, FERRY_CAPACITY, ferry_pid);

                    if (ferry_pid > 1 && ferry_pid != getpid()) {
                        if (kill(ferry_pid, SIGUSR1) == -1) {
                            perror("[CAPTAIN PORT] kill SIGUSR1 failed");
                        } else {
                            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] SIGUSR1 sent successfully\n");
                            signal_sent_for_current_ferry = true;
                        }
                    } else {
                        dprintf(STDERR_FILENO,
                            "[ERROR] invalid ferry_pid=%d, signal NOT sent\n",
                            ferry_pid);
                    }
                }
            }
            
            usleep(100000);
            continue;
        }

        dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Passenger id=%d baggage=%d kg\n", 
                msg.passenger_id, msg.baggage_weight);

        DecisionMessage decision;
        decision.mtype = MSG_TYPE_DECISION_BASE + msg.passenger_id;
        decision.passenger_id = msg.passenger_id;

        if (msg.baggage_weight > MAX_BAGGAGE) {
            decision.accepted = 0;
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Passenger id=%d REJECTED (baggage too heavy)\n", 
                    msg.passenger_id);
        } else {
            decision.accepted = 1;
            accepted_count++;
            current_ferry_passengers++;
            
            dprintf(STDOUT_FILENO, "[CAPTAIN PORT] Passenger id=%d ACCEPTED (ferry: %d/%d)\n", 
                    msg.passenger_id, current_ferry_passengers, FERRY_CAPACITY);
        }

        msgsnd(msgid, &decision, sizeof(DecisionMessage) - sizeof(long), 0);        
    }
}