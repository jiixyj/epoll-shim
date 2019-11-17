#include <unistd.h>

extern int real_close(int fd);

int
real_close(int fd)
{
	return close(fd);
}
