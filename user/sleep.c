#include "kernel/types.h"
#include "user/user.h"

void usage_err()
{
    fprintf(2, "Usage: sleep [x100 milliseconds]\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc != 2)
        usage_err();

    int seconds = atoi(argv[1]);
    sleep(seconds);

    exit(0);
}
