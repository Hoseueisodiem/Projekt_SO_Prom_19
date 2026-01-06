#pragma once
#include <sys/types.h>

#define MSG_TYPE_PASSENGER 1

struct PassengerMessage {
    long mtype;
    int passenger_id;
    pid_t pid;
};

