#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
int main()
{
    close(1);
    int fd = open("file.txt", O_WRONLY | O_CREAT);
    printf("%d\n", fd);
}