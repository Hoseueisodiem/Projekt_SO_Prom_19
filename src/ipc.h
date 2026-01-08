#pragma once
#include <sys/types.h>

#define MSG_TYPE_PASSENGER 1
#define MSG_TYPE_DECISION 2

struct PassengerMessage {
    long mtype;
    int passenger_id;
    pid_t pid;
    int baggage_weight; // kg
};

struct DecisionMessage {
    long mtype;
    int passenger_id;
    int accepted; // 1 ok, 0 odrzucenie
};

