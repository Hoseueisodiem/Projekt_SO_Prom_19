#include <cstdio>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include "captain_port.h"
#include "ipc.h"

void run_captain_port() {
    key_t key = ftok(".", 'P');
    int msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid == -1) {
        dprintf(STDERR_FILENO,
                "[CAPTAIN PORT] failed to create message queue\n");
        _exit(1);
    }

    dprintf(STDOUT_FILENO,
        "[CAPTAIN PORT] PID=%d waiting for passengers...\n",
        getpid());

    PassengerMessage msg;
    while (true){
        msgrcv(msgid, &msg, sizeof(PassengerMessage) -sizeof(long),
            MSG_TYPE_PASSENGER, 0);

        dprintf(STDOUT_FILENO,
                "[CAPTAIN PORT] Received passenger id=%d pid=%d\n",
                msg.passenger_id,
                msg.pid);
    }
}
