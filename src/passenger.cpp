#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include <cstdio>

#include "passenger.h"
#include "ipc.h"

void run_passenger(int id) {
    key_t key = ftok(".", 'P');
    int msgid = msgget(key, 0666);
    if (msgid == -1) {
        dprintf(STDERR_FILENO,
                "[PASSENGER] id=%d failed to get message queue\n", id);
        _exit(1);
    }

    PassengerMessage msg;
    msg.mtype = MSG_TYPE_PASSENGER;
    msg.passenger_id = id;
    msg.pid = getpid();

    msgsnd(msgid, &msg, sizeof(PassengerMessage) -sizeof(long), 0);

    dprintf(STDOUT_FILENO,
            "[PASSENGER] id=%d sent request to captain port\n", id);

}
