#include "stdio.h"

int main(int argc, char * argv[])
{
	printf("Starting desktop environment...\n");
	printf("Press ESC to exit GUI mode\n");

	desktop_start();

	printf("[desktop finished]\n");
	return 0;
}
