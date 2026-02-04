#include <cstdio>
#include <unistd.h>
#include <sys/msg.h>
#include <sys/wait.h>

struct msg_buffer {
    long mtype;
    int value;
};

int main() {
    printf("TEST: Message queue send/receive\n");
    
    key_t key = ftok("/tmp", 'Q');
    int msgid = msgget(key, IPC_CREAT | 0666);
    
    if (fork() == 0) {
        msg_buffer msg = {1, 123};
        msgsnd(msgid, &msg, sizeof(int), 0);
        printf("Child: sent value=%d\n", msg.value);
        _exit(0);
    }
    
    wait(NULL);
    
    msg_buffer msg;
    msgrcv(msgid, &msg, sizeof(int), 1, 0);
    printf("Parent: received value=%d\n", msg.value);
    
    if (msg.value == 123) {
        printf("PASS: Message queue worked correctly\n");
    } else {
        printf("FAIL: Expected 123, got %d\n", msg.value);
    }
    
    msgctl(msgid, IPC_RMID, nullptr);
    return 0;
}