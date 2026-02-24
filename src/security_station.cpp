#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <sys/prctl.h>
#include <cerrno>

#include "security_station.h"
#include "security.h"
#include "ipc.h"

static void sem_down(int semid, int semnum) {
    struct sembuf sb = {static_cast<unsigned short>(semnum), -1, 0};
    semop(semid, &sb, 1);
}

static void sem_up(int semid, int semnum) {
    struct sembuf sb = {static_cast<unsigned short>(semnum), 1, 0};
    semop(semid, &sb, 1);
}

// pasazer czekajacy w kolejce na wejscie do stanowiska
struct WaitingEntry {
    int passenger_id;
    int gender;
    int vip;
    int skip_count;  // ile razy zostal przepuszczony przez kogos innego
};

// pasazer aktualnie kontrolowany na stanowisku
struct InspectionEntry {
    int passenger_id;
    struct timespec admit_time;  // kiedy weszl na stanowisko
    int dangerous_item;         // 1 jesli ma niebezpieczny przedmiot
};

// roznica czasu w ms miedzy dwoma timespec
static long elapsed_ms(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) * 1000L
         + (end.tv_nsec - start.tv_nsec) / 1000000L;
}

void run_security_station(int station_id) {
    prctl(PR_SET_NAME, "kontrola_bezp");
    srand(getpid());

    // dostep do kolejki kontroli (tworzona przez kapitan_port)
    key_t sec_key = ftok("/tmp", 'Q');
    int sec_queue = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        sec_queue = msgget(sec_key, 0666);
        if (sec_queue != -1) break;
        usleep(100000);
    }
    if (sec_queue == -1) {
        perror("[STATION] msgget sec_queue");
        _exit(1);
    }

    // dostep do PortState (shared memory)
    key_t port_state_key = ftok("/tmp", 'S');
    int port_shmid = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        port_shmid = shmget(port_state_key, sizeof(PortState), 0666);
        if (port_shmid != -1) break;
        usleep(100000);
    }
    if (port_shmid == -1) {
        perror("[STATION] shmget port_state");
        _exit(1);
    }
    PortState* port_state = (PortState*) shmat(port_shmid, nullptr, 0);
    if (port_state == (void*) -1) {
        perror("[STATION] shmat port_state");
        _exit(1);
    }

    // dostep do mutexu
    key_t mutex_key = ftok("/tmp", 'X');
    int mutex = -1;
    for (int attempt = 0; attempt < 50; attempt++) {
        mutex = semget(mutex_key, 1, 0666);
        if (mutex != -1) break;
        usleep(100000);
    }
    if (mutex == -1) {
        perror("[STATION] semget mutex");
        _exit(1);
    }

    // Stan wewnetrzny stanowiska (zarzadzany wylacznie przez ten proces)
    int current_count = 0;  // ile osob teraz na stanowisku (0-2)
    int current_gender = -1; // plec aktualnie kontrolowanych (-1 = brak)
    std::vector<WaitingEntry>    waiting;         // kolejka oczekujacych
    std::vector<InspectionEntry> being_inspected;  // aktualnie kontrolowani

    dprintf(STDOUT_FILENO, "[STATION %d] PID=%d Ready\n", station_id, getpid());

    // licznik iteracji z pustymi kolejkami po zamknieciu portu
    // czas na doarcie pasazerow ktorzy sa w drodze
    int idle_after_close = 0;

    while (true) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        // sprawdz zakonczonych inspekcji
        std::vector<InspectionEntry> still_inspecting;
        for (auto& entry : being_inspected) {
            if (elapsed_ms(entry.admit_time, now) >= INSPECTION_TIME_MS) {
                // inspekcja zakonczona wyslij SecurityDoneMsg do pasazera
                SecurityDoneMsg done_msg;
                done_msg.mtype = MSG_TYPE_SECURITY_DONE_BASE + entry.passenger_id;
                done_msg.passenger_id = entry.passenger_id;
                done_msg.station_id = station_id;
                done_msg.dangerous_item_found = entry.dangerous_item;

                msgsnd(sec_queue, &done_msg,
                       sizeof(SecurityDoneMsg) - sizeof(long), 0);

                // zaktualizuj licznik w PortState
                sem_down(mutex, 0);
                if (port_state->passengers_in_security > 0)
                    port_state->passengers_in_security--;
                sem_up(mutex, 0);

                // zaktualizuj stan stanowiska
                current_count--;
                if (current_count == 0) current_gender = -1;

                if (entry.dangerous_item) {
                    dprintf(STDOUT_FILENO,
                        "[STATION %d] DANGEROUS ITEM found on passenger %d! "
                        "Item confiscated, passenger continues.\n",
                        station_id, entry.passenger_id);
                } else {
                    dprintf(STDOUT_FILENO,
                        "[STATION %d] Passenger %d OK, inspection complete\n",
                        station_id, entry.passenger_id);
                }
            } else {
                still_inspecting.push_back(entry);
            }
        }
        being_inspected = still_inspecting;

        // wpuszczaj oczekujacych (max 2, ta sama plec)
        while (current_count < 2 && !waiting.empty()) {
            int selected = -1;

            // Priorytet 1 pasazerowie z skip_count >= 3 o odpowiedniej plci
            for (int i = 0; i < (int)waiting.size(); i++) {
                auto& w = waiting[i];
                bool gender_ok = (current_count == 0) || (w.gender == current_gender);
                if (gender_ok && w.skip_count >= 3) {
                    selected = i;
                    break;
                }
            }

            // Priorytet 2 FIFO wsrod uprawnionych (ta sama plec lub puste stanowisko)
            if (selected == -1) {
                for (int i = 0; i < (int)waiting.size(); i++) {
                    auto& w = waiting[i];
                    bool gender_ok = (current_count == 0) || (w.gender == current_gender);
                    if (gender_ok) {
                        selected = i;
                        break;
                    }
                }
            }

            if (selected == -1) break;  // brak uprawnionego pasazera

            WaitingEntry admitted = waiting[selected];
            waiting.erase(waiting.begin() + selected);

            // ustaw plec stanowiska jesli bylo puste
            if (current_count == 0) current_gender = admitted.gender;
            current_count++;

            // zwieksz skip_count pozostalym oczekujacym tej samej plci
            // (zostali przepuszczeni przez admitted)
            for (auto& w : waiting) {
                if (w.gender == current_gender) {
                    w.skip_count++;
                    if (w.skip_count == 3) {
                        dprintf(STDOUT_FILENO,
                            "[STATION %d] Passenger %d: PRIORITY granted after 3 skips\n",
                            station_id, w.passenger_id);
                    }
                }
            }

            // dodaj do aktywnej inspekcji z losowym wynikiem niebezp przedmiotu
            InspectionEntry insp;
            insp.passenger_id = admitted.passenger_id;
            clock_gettime(CLOCK_MONOTONIC, &insp.admit_time);
            insp.dangerous_item = (rand() % 100 < DANGEROUS_ITEM_CHANCE) ? 1 : 0;
            being_inspected.push_back(insp);

            dprintf(STDOUT_FILENO,
                "[STATION %d] Passenger %d %sENTER (at station: %d, "
                "skip=%d, queue: %zu)\n",
                station_id, admitted.passenger_id,
                admitted.skip_count >= 3 ? "(PRIORITY) " : "",
                current_count, admitted.skip_count, waiting.size());
        }

        // Odbierz nowe zgloszenia
        SecurityJoinMsg join_msg;
        while (msgrcv(sec_queue, &join_msg,
                      sizeof(SecurityJoinMsg) - sizeof(long),
                      station_id + 1,   // mtype = station_id+1 (1,2,3)
                      IPC_NOWAIT) != -1) {
            WaitingEntry w;
            w.passenger_id = join_msg.passenger_id;
            w.gender       = join_msg.gender;
            w.vip          = join_msg.vip;
            w.skip_count   = 0;
            waiting.push_back(w);
            dprintf(STDOUT_FILENO,
                "[STATION %d] Passenger %d joined queue "
                "(queue: %zu, inspecting: %zu)\n",
                station_id, join_msg.passenger_id,
                waiting.size(), being_inspected.size());
        }

        // war zakonczenia
        sem_down(mutex, 0);
        bool port_closed = !port_state->accepting_passengers;
        int in_sec = port_state->passengers_in_security;
        sem_up(mutex, 0);

        if (port_closed && waiting.empty() && being_inspected.empty()) {
            // jesli passengers_in_security == 0, wszystkie stanowiska skonczone
            if (in_sec == 0) {
                idle_after_close++;
                // Kilka dodatkowych iteracji na wypadek pasazerow w drodze
                if (idle_after_close >= 20) {
                    dprintf(STDOUT_FILENO,
                        "[STATION %d] Port closed, queue empty. Shutting down.\n",
                        station_id);
                    break;
                }
            } else {
                idle_after_close = 0;  // sa jeszcze pasazerowie na innych stanowiskach
            }
        } else {
            idle_after_close = 0;
        }

        usleep(50000);  // 50ms petla glowna
    }

    shmdt(port_state);
    dprintf(STDOUT_FILENO, "[STATION %d] Shutdown complete\n", station_id);
}
