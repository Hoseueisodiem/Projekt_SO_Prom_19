#pragma once
#include <sys/types.h>

#define NUM_STATIONS 3
#define MALE   0
#define FEMALE 1

struct SecurityStation {
    int count;   // ile osob przy stanowisku
    int gender;  // MALE / FEMALE, -1 = brak
};