#include "kernel/types.h"
#include "user/user.h"

#define stdin 0
#define stdout 1
#define stderr 2

int myfork(int function())
{
    int pid = fork();
    if (pid==0)
        exit(function());
    return pid;
}

int parent_child[2], child_parent[2];
int child()
{
    uint8 buf[256]={0};
    read(parent_child[0], buf, sizeof(buf));
    fprintf(stdout, "%d: received ping\n", getpid());
    uint8 pong_data[] = {0x50};
    write(child_parent[1], pong_data, sizeof(pong_data));
    exit(0);
}

int main()
{
    pipe(parent_child);
    pipe(child_parent);
    myfork(child);

    uint8 ping_data[] = {0x53};
    write(parent_child[1],  ping_data, sizeof(ping_data));

    uint8 buf[256]={0};
    read(child_parent[0], buf, sizeof(buf));
    fprintf(stdout, "%d: received pong\n", getpid());

    exit(0);
}