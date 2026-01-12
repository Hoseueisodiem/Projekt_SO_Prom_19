#pragma once
#include <sys/types.h>

#define NUM_STATIONS 3
#define MALE   0
#define FEMALE 1

#define FERRY_CAPACITY 5    // P - max pasazerow na promie
#define GANGWAY_CAPACITY 2  // K - max osob na trapie

#define DEPARTURE_TIME 10   // T1 - co ile sekund odplywa prom

struct SecurityStation {
    int count;   // ile osob przy stanowisku
    int gender;  // MALE / FEMALE, -1 = brak
};
struct FerryState {
    int onboard;  // ilu pasazerow jest na promie
};