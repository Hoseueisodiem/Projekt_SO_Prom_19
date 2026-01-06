#include <iostream>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/ipc.h>
#include "passenger.h"
#include "ipc.h"

void run_passenger(int id) {
    key_t key = ftok(".",'p');
    int msgid = msgget(key, 0666);

    PassengerMessage msg;
    msg.mtype = MSG_TYPE_PASSENGER;
    msg.passenger_id = id;
    msg.pid = getpid();

    msgsnd(msgid, &msg, sizeof(PassengerMessage) -sizeof(long), 0);

    std::cout << "[PASSENGER] id=" << id
              << " sent request to captain port" << std::endl;
    std::cout << "[PASSENGER] id=" << id
          << " sent request to captain port" << std::endl;
std::cout.flush();

}
