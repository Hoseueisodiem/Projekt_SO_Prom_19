#include <iostream>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include "captain_port.h"
#include "ipc.h"

void run_captain_port() {
    key_t key = ftok(".", 'P');
    int msgid = msgget(key, IPC_CREAT | 0666);

    std::cout << "[CAPTAIN PORT] PID=" << getpid()
              << " waiting for passengers..." << std::endl;

    PassengerMessage msg;
    while (true){
        msgrcv(msgid, &msg, sizeof(PassengerMessage) -sizeof(long),
            MSG_TYPE_PASSENGER, 0);

        std::cout << "[CAPTAIN PORT] Received passenger id="
                  << msg.passenger_id
                  << " pid=" << msg.pid << std::endl; 
        std::cout.flush();
    }
}
