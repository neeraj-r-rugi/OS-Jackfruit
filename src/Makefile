CC = gcc
src = $(wildcard *.c)
target = $(src:.c=)

all:
	$(CC) -o $(target) $(src) -lpthread
clean:
	rm -f $(target)