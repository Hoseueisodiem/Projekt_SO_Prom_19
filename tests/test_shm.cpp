#include <cstdio>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/wait.h>

int main() {
    printf("TEST: Shared memory communication\n");
    
    key_t key = ftok("/tmp", 'S');
    int shmid = shmget(key, sizeof(int), IPC_CREAT | 0666);
    int* shared = (int*) shmat(shmid, nullptr, 0);
    
    *shared = 0;
    printf("Parent: wrote value=%d\n", *shared);
    
    if (fork() == 0) {
        printf("Child: read value=%d\n", *shared);
        *shared = 42;
        printf("Child: wrote value=%d\n", *shared);
        shmdt(shared);
        _exit(0);
    }
    
    wait(NULL);
    printf("Parent: read value=%d (after child modified)\n", *shared);
    
    if (*shared == 42) {
        printf("PASS: Shared memory worked correctly\n");
    } else {
        printf("FAIL: Expected 42, got %d\n", *shared);
    }
    
    shmdt(shared);
    shmctl(shmid, IPC_RMID, nullptr);
    return 0;
}