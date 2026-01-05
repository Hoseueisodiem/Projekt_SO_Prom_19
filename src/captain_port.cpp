#include <iostream>
#include <unistd.h>
#include "captain_port.h"

void run_captain_port() {
    std::cout << "[CAPTAIN PORT] PID=" << getpid() << std::endl;
}
