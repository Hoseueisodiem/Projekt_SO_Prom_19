#pragma once
#include <sys/types.h>

#define MSG_TYPE_PASSENGER 1
#define MSG_TYPE_DECISION_BASE 1000
#define MSG_TYPE_FERRY_DEPARTED 2000

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
    int ferry_id; // id przydzielanego promu (-1 odrzucenie)
};

struct FerryDepartureMessage {
    long mtype;
    int ferry_id;
    int passengers_count;  // ilu pasażerów zabrał
};

