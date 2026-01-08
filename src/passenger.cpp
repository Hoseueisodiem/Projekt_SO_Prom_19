#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "passenger.h"
#include "ipc.h"

void run_passenger(int id) {
    key_t key = ftok(".", 'P');
    int msgid = msgget(key, 0666);
    if (msgid == -1) {
        dprintf(STDERR_FILENO, "[PASSENGER] id=%d failed to get message queue\n", id);
        _exit(1);
    }

    srand(getpid());
    int baggage = rand() % 40 + 1; //1-40kg

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
           MSG_TYPE_DECISION,
           0);

    if (decision.accepted) {
        dprintf(STDOUT_FILENO, "[PASSENGER] id=%d ACCEPTED\n", id);
    } else {
        dprintf(STDOUT_FILENO, "[PASSENGER] id=%d REJECTED (baggage too heavy)\n", id);
    }
}