#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"

#define stdin 0
#define stdout 1
#define stderr 2

char readstr(char *str, int size)
{
    int p;
    for (p = 0; p < size; p++)
    {
        if (read(stdin, str + p, 1) == 0)
            return 2;
        if (str[p] == '\n')
        {
            str[p] = 0;
            return 1;
        }
        if (str[p] == ' ')
        {
            str[p] = 0;
            return 0;
        }
    }
    return 3;
}

#define MAXARGSIZE 32

int main(int argc, char *argv[])
{
    int i;

    char *args[MAXARG];
    // 复制本身的参数
    memcpy(args, argv + 1, sizeof(char *) * (argc - 1));
    while (1)
    {
        // 从stdin输入参数
        char arg[MAXARG][MAXARGSIZE] = {0};
        for (i = argc - 1; i < MAXARG; i++)
        {
            args[i] = arg[i];
            int tag = readstr(arg[i], MAXARGSIZE);
            //  如果是一行的结尾
            if (tag == 1)
            {
                args[i + 1] = 0;
                break;
            }
            // 如果是流的结尾
            if (tag == 2)
                break;
        }
        // 如果没有读入任何字符串
        if (arg[argc - 1][0] == 0)
            break;

        int pid = fork();
        if (pid==0)
        {
            exec(args[0], args);
            printf("execve failed");
            exit(0);
        }
        wait(&pid);
    }
    exit(0);
}