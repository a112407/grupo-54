#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
	(void)argv;

	if (argc < 2) {
		fprintf(stderr, "Uso: ./runner <args>\n");
		return 1;
	}

	return 0;
}
