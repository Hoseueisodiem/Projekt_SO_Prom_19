#pragma once
#include <sys/types.h>

#define NUM_STATIONS 3
#define MALE   0
#define FEMALE 1

#define FERRY_CAPACITY 5    // P - max pasazerow na promie
#define GANGWAY_CAPACITY 2 // K - max osob na trapie

#define DEPARTURE_TIME 10   // T1 - co ile sekund odplywa prom
#define TRAVEL_TIME 5       // Ti - czas podrozy tam i z powrotem

struct SecurityStation {
    int count;   // ile osob przy stanowisku
    int gender;  // MALE / FEMALE, -1 = brak
};
struct FerryState {
    int onboard;  // ilu pasazerow jest na promie
    int in_waiting; // ilu czeka w poczekalni po kontroli, przed promem
};
struct PortState {
    int accepting_passengers;  // 1 otwarty, 0 zamkniety
    int passengers_onboard;    // ilu pasazerow jest aktualnie na wszystkich promach
    pid_t ferry_captain_pid;   // PID kapitana promu
};