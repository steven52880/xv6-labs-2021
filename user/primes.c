#include "kernel/types.h"
#include "user/user.h"

#define stdin 0
#define stdout 1
#define stderr 2

#define pipe_read 0
#define pipe_write 1

/*
踩坑：
第一次写的时候用了递归，实际上没有必要。递归会导致还没有释放前一个管道就申请下一个了，然后管道数量超过上限。
主要是算法介绍页面上面的图和介绍让我看着就想到递归。实际上甚至不用fork都行。

注意这里，管道未关闭，从pipe中read会阻塞等待数据；写入一端的管道关闭后，数据全部读取完会直接立刻返回0
*/

typedef struct task_struct
{
    int num;
    int pipe_in;
    int pipe_out;
} task_struct;

int myfork(void function(task_struct data), task_struct data)
{
    int pid = fork();
    if (pid == 0)
    {
        function(data);
        exit(0);
    }
    return pid;
}

void app_error(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

task_struct newdata;

void prime(task_struct data)
{
    while (1)
    {
        int num;
        if (read(data.pipe_in, &num, sizeof(num)) == 0)
            break;
        if (num % data.num != 0)
        {
            write(data.pipe_out, &num, sizeof(num));
            //printf("%d %% %d\n", num, data.num);
        }
    }
    exit(0);
}

int main()
{
    task_struct data;
    int fd[2];
    pipe(fd);
    data.pipe_out = fd[pipe_write];

    int i;
    for (i = 2; i <= 35; i++)
        write(data.pipe_out, &i, sizeof(i));
    close(data.pipe_out);

    while (1)
    {
        //sleep(1);
        data.pipe_in = fd[pipe_read];
        pipe(fd);
        data.pipe_out = fd[pipe_write];

        if (read(data.pipe_in, &data.num, sizeof(data.num)))
        {
            printf("prime %d\n", data.num);
        }
        else
        {
            close(data.pipe_in);
            close(data.pipe_out);
            break;
        }

        myfork(prime, data);
        int pid;
        wait(&pid);

        close(data.pipe_in);
        close(data.pipe_out);
    }
    exit(0);
}
