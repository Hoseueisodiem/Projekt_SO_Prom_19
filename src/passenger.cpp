#include <iostream>
#include <unistd.h>
#include "passenger.h"

void run_passenger(int id) {
    std::cout << "[PASSENGER] PID=" << getpid()
        << " id=" << id << std::endl;
}
