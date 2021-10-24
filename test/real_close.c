#include <unistd.h>

extern int real_close_for_test(int fd);

int
real_close_for_test(int fd)
{
	return close(fd);
}
