CC = gcc
CFLAGS = -Wall -Wextra -g

test: allocator.c allocTest.c
	$(CC) $(CFLAGS) allocator.c allocTest.c -o test
	./test

clean:
	rm -f test hw6