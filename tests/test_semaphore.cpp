#include <cstdio>
#include <unistd.h>
#include <sys/sem.h>
#include <sys/wait.h>

int main() {
    printf("TEST: Semaphore blocks second process\n");
    
    key_t key = ftok("/tmp", 'T');
    int semid = semget(key, 1, IPC_CREAT | 0666);
    semctl(semid, 0, SETVAL, 1);
    
    struct sembuf down = {0, -1, 0};
    struct sembuf up = {0, 1, 0};
    
    semop(semid, &down, 1);
    printf("Parent: acquired semaphore\n");
    
    if (fork() == 0) {
        printf("Child: trying to acquire semaphore\n");
        semop(semid, &down, 1);
        printf("Child: acquired semaphore (after parent released)\n");
        semop(semid, &up, 1);
        _exit(0);
    }
    
    sleep(2);
    printf("Parent: releasing semaphore\n");
    semop(semid, &up, 1);
    
    wait(NULL);
    semctl(semid, 0, IPC_RMID);
    printf("PASS: Semaphore worked correctly\n");
    return 0;
}