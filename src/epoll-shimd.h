struct eventfd_setup {
	unsigned int initval;
	int flags;
};

#define EVENTFD_IOCTL_SETUP _IOW('v', 1, struct eventfd_setup)
