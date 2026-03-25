CC = gcc
src = $(wildcard *.c)
target = $(src:.c=)

all:
	$(CC) -o $(target) $(src)
clean:
	rm -f $(target)