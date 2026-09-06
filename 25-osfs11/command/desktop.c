#include "stdio.h"

int main(int argc, char * argv[])
{
	printf("Starting desktop environment...\n");
	printf("Press ESC to exit GUI mode\n");

	if (desktop_start() != 0) {
		printf("Cannot start desktop: graphics device is busy.\n");
		return 1;
	}

	printf("[desktop finished]\n");
	return 0;
}
