#pragma once
#include <sys/types.h>

#define NUM_STATIONS 3
#define MALE   0
#define FEMALE 1

#define NUM_FERRIES 3       // liczba promów
#define FERRY_CAPACITY 5    // P - max pasazerow na promie
#define GANGWAY_CAPACITY 2 // K - max osob na trapie

#define DEPARTURE_TIME 10   // T1 - co ile sekund odplywa prom
#define TRAVEL_TIME 20       // Ti - czas podrozy tam i z powrotem
#define MAX_BAGGAGE 20

enum FerryStatus {
    FERRY_AVAILABLE,   // dostepny do zaladowania
    FERRY_BOARDING,    // pasazerowie wsiadaja
    FERRY_TRAVELING,   // w podrozy
    FERRY_SHUTDOWN     // zakonczony po sigusr2
};

struct SecurityStation {
    int count;   // ile osob przy stanowisku
    int gender;  // MALE / FEMALE, -1 = brak
};
// prom pojedyczny
struct Ferry {
    int onboard;           // ile na promie
    int in_waiting;        // ile w poczekalni
    int capacity;          // pojemnosc Mp
    int baggage_limit;     // limit bagazu w kg
    FerryStatus status;    // prom status
    pid_t captain_pid;     // PID kapitana tego promu
    bool signal_sent;      // czy wyslano sigusr1
};
// stan portu
struct PortState {
    int accepting_passengers;    // 1 = port otwarty, 0 = zamkniety
    int passengers_onboard;      // liczba pasazerow na wszystkich promach
    Ferry ferries[NUM_FERRIES];  // tablica wszystkich promow
};