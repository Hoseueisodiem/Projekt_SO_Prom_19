#include <iostream>
#include <unistd.h>
#include "captain_ferry.h"

void run_captain_ferry() {
    std::cout << "[CAPTAIN FERRY] PID=" << getpid() << std::endl;
}
