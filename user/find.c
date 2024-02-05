#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/stat.h"
#include "user/user.h"

int find(const char *dir, const char *filename)
{
    // 打开当前目录
    int fd;
    if ((fd = open(dir, 0)) < 0)
    {
        fprintf(2, "ls: cannot open %s\n", dir);
        return 0;
    }

    // 判断是否是文件
    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        fprintf(2, "ls: cannot stat %s\n", dir);
        close(fd);
        return 0;
    }
    if (st.type !=T_DIR)
    {
        close(fd);
        return 0;
    }

    // 拼凑当前目录路径
    char pathbuf[512];
    char *p = pathbuf;

    strcpy(pathbuf, dir);
    p += strlen(pathbuf);

    strcpy(p, "/");
    p++;

    // printf("%s, %d\n", pathbuf, p - pathbuf);

    // 遍历目录
    int count = 0;
    struct dirent de;
    while (read(fd, &de, sizeof(de)) == sizeof(de))
    {
        if (de.inum == 0)
            continue;
        // 排除当前目录、父目录
        if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
            continue;

        // 拼接下一级路径
        strcpy(p, de.name);
        //printf("%s\n", pathbuf);

        // 判断是否找到
        if (strcmp(de.name, filename) == 0)
        {
            count++;
            printf("%s\n", pathbuf);
        }

        // 递归调用
        find(pathbuf, filename);
    }
    close(fd);
    return count;
}

int main(int argc, char *argv[])
{
    if (argc<3)
    {
        printf("Usage: find [path] [name]\n");
        exit(0);
    }
    find(argv[1], argv[2]);
    exit(0);
}